/**
 * @file argus_nvs_config.c
 * @brief Power-Loss-Safe Dual-Slot NVS Configuration Manager Implementation
 */

#include "argus_nvs_config.h"
#include "argus_identity.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define ARGUS_NVS_NS_CFG        "argus_cfg"
#define ARGUS_NVS_NS_SYS        "argus_sys"
#define ARGUS_NVS_KEY_SLOT_0    "slot_a"
#define ARGUS_NVS_KEY_SLOT_1    "slot_b"
#define ARGUS_NVS_KEY_SELECTOR  "active_slot"
#define ARGUS_NVS_KEY_RESET_PEND "rst_pend"

static argus_config_payload_t s_active_config;
static uint32_t s_active_generation = 0;
static uint8_t s_active_slot_index = 0;
static bool s_has_valid_config = false;
static bool s_initialized = false;

static const argus_nvs_driver_t *s_custom_driver = NULL;

// Standard IEEE 802.3 CRC32 pure C implementation for cross-platform portability
uint32_t argus_nvs_config_calc_crc32(const argus_config_payload_t *payload)
{
    if (!payload) return 0;
    const uint8_t *data = (const uint8_t *)payload;
    size_t len = sizeof(argus_config_payload_t);
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc = (crc >> 1);
            }
        }
    }
    return ~crc;
}

bool argus_nvs_config_gen_is_newer(uint32_t gen_a, uint32_t gen_b)
{
    if (gen_a == 0) return false;
    if (gen_b == 0) return true;
    return ((int32_t)(gen_a - gen_b)) > 0;
}

static bool is_slot_valid(const argus_cfg_slot_t *slot)
{
    if (!slot) return false;
    if (slot->schema_version != ARGUS_CONFIG_SCHEMA_VERSION) return false;
    if (slot->valid_marker != ARGUS_CONFIG_VALID_MARKER) return false;
    if (slot->config_generation == 0) return false;
    if (slot->payload_length != sizeof(argus_config_payload_t)) return false;
    uint32_t computed_crc = argus_nvs_config_calc_crc32(&slot->payload);
    if (computed_crc != slot->crc32) return false;
    return (argus_nvs_config_validate(&slot->payload) == ESP_OK);
}

// Default production NVS storage driver implementations
static esp_err_t prod_read_slot(uint8_t slot_index, argus_cfg_slot_t *out_slot)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    const char *key = (slot_index == 0) ? ARGUS_NVS_KEY_SLOT_0 : ARGUS_NVS_KEY_SLOT_1;
    size_t len = sizeof(argus_cfg_slot_t);
    err = nvs_get_blob(handle, key, out_slot, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t prod_write_slot(uint8_t slot_index, const argus_cfg_slot_t *in_slot)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    const char *key = (slot_index == 0) ? ARGUS_NVS_KEY_SLOT_0 : ARGUS_NVS_KEY_SLOT_1;
    err = nvs_set_blob(handle, key, in_slot, sizeof(argus_cfg_slot_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t prod_read_selector(uint8_t *out_selector)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_u8(handle, ARGUS_NVS_KEY_SELECTOR, out_selector);
    nvs_close(handle);
    return err;
}

static esp_err_t prod_write_selector(uint8_t selector)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, ARGUS_NVS_KEY_SELECTOR, selector);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t prod_read_reset_pending(bool *out_pending)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_SYS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        *out_pending = false;
        return ESP_OK;
    }

    uint8_t val = 0;
    err = nvs_get_u8(handle, ARGUS_NVS_KEY_RESET_PEND, &val);
    nvs_close(handle);
    *out_pending = (err == ESP_OK && val == 1);
    return ESP_OK;
}

static esp_err_t prod_write_reset_pending(bool pending)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_SYS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, ARGUS_NVS_KEY_RESET_PEND, pending ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t prod_erase_all(void)
{
    nvs_handle_t handle;
    if (nvs_open(ARGUS_NVS_NS_CFG, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    if (nvs_open(ARGUS_NVS_NS_SYS, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    return ESP_OK;
}

static const argus_nvs_driver_t s_prod_driver = {
    .read_slot = prod_read_slot,
    .write_slot = prod_write_slot,
    .read_selector = prod_read_selector,
    .write_selector = prod_write_selector,
    .read_reset_pending = prod_read_reset_pending,
    .write_reset_pending = prod_write_reset_pending,
    .erase_all = prod_erase_all
};

static const argus_nvs_driver_t *get_driver(void)
{
    return s_custom_driver ? s_custom_driver : &s_prod_driver;
}

esp_err_t argus_nvs_config_init(const argus_nvs_driver_t *driver)
{
    s_custom_driver = driver;

    if (!s_custom_driver) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
    }

    const argus_nvs_driver_t *drv = get_driver();

    // Check for power-interrupted factory reset recovery
    bool reset_pending = false;
    drv->read_reset_pending(&reset_pending);
    if (reset_pending) {
        drv->erase_all();
        drv->write_reset_pending(false);
    }

    // Load dual slots
    argus_cfg_slot_t slot_a = {0}, slot_b = {0};
    bool valid_a = (drv->read_slot(0, &slot_a) == ESP_OK) && is_slot_valid(&slot_a);
    bool valid_b = (drv->read_slot(1, &slot_b) == ESP_OK) && is_slot_valid(&slot_b);

    uint8_t selector = 0xFF;
    drv->read_selector(&selector);

    if (valid_a && valid_b) {
        if (selector == 0) {
            s_active_slot_index = 0;
            memcpy(&s_active_config, &slot_a.payload, sizeof(argus_config_payload_t));
            s_active_generation = slot_a.config_generation;
            s_has_valid_config = true;
        } else if (selector == 1) {
            s_active_slot_index = 1;
            memcpy(&s_active_config, &slot_b.payload, sizeof(argus_config_payload_t));
            s_active_generation = slot_b.config_generation;
            s_has_valid_config = true;
        } else {
            // Corrupt selector: fall back to generation comparison
            if (argus_nvs_config_gen_is_newer(slot_b.config_generation, slot_a.config_generation)) {
                s_active_slot_index = 1;
                memcpy(&s_active_config, &slot_b.payload, sizeof(argus_config_payload_t));
                s_active_generation = slot_b.config_generation;
            } else {
                s_active_slot_index = 0;
                memcpy(&s_active_config, &slot_a.payload, sizeof(argus_config_payload_t));
                s_active_generation = slot_a.config_generation;
            }
            s_has_valid_config = true;
            drv->write_selector(s_active_slot_index);
        }
    } else if (valid_a) {
        s_active_slot_index = 0;
        memcpy(&s_active_config, &slot_a.payload, sizeof(argus_config_payload_t));
        s_active_generation = slot_a.config_generation;
        s_has_valid_config = true;
    } else if (valid_b) {
        s_active_slot_index = 1;
        memcpy(&s_active_config, &slot_b.payload, sizeof(argus_config_payload_t));
        s_active_generation = slot_b.config_generation;
        s_has_valid_config = true;
    } else {
        // Uncommissioned defaults
        memset(&s_active_config, 0, sizeof(argus_config_payload_t));
        snprintf(s_active_config.client_id, sizeof(s_active_config.client_id), "default_client");
        snprintf(s_active_config.unit_id, sizeof(s_active_config.unit_id), "unit_01");
        snprintf(s_active_config.device_name, sizeof(s_active_config.device_name), "Argus Peristaltic Pump V2");
        s_active_generation = 0;
        s_has_valid_config = false;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t argus_nvs_config_get(argus_config_payload_t *out_cfg)
{
    if (!out_cfg) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) argus_nvs_config_init(NULL);
    memcpy(out_cfg, &s_active_config, sizeof(argus_config_payload_t));
    return s_has_valid_config ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t argus_nvs_config_validate(const argus_config_payload_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    if (!argus_identity_validate_client_id(cfg->client_id)) return ESP_ERR_INVALID_ARG;
    if (!argus_identity_validate_unit_id(cfg->unit_id)) return ESP_ERR_INVALID_ARG;
    if (!argus_identity_validate_device_name(cfg->device_name)) return ESP_ERR_INVALID_ARG;

    size_t ssid_len = strlen(cfg->sta_ssid);
    if (ssid_len > ARGUS_CFG_STA_SSID_MAX) return ESP_ERR_INVALID_ARG;

    size_t pass_len = strlen(cfg->sta_pass);
    // If STA SSID is populated, WPA2 password MUST be 8..63 chars. Open STA is rejected in Schema V1.
    if (ssid_len > 0) {
        if (pass_len < ARGUS_CFG_STA_PASS_MIN || pass_len > ARGUS_CFG_STA_PASS_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

bool argus_nvs_config_is_commissioned(const argus_config_payload_t *cfg)
{
    if (argus_nvs_config_validate(cfg) != ESP_OK) return false;
    if (strlen(cfg->sta_ssid) == 0) return false;
    if (strlen(cfg->sta_pass) < ARGUS_CFG_STA_PASS_MIN) return false;
    return true;
}

esp_err_t argus_nvs_config_commit(const argus_config_payload_t *in_cfg)
{
    if (!in_cfg) return ESP_ERR_INVALID_ARG;

    // Reject mask strings submitted as real credentials
    if (strcmp(in_cfg->sta_pass, ARGUS_CONFIG_MASK_STRING) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = argus_nvs_config_validate(in_cfg);
    if (err != ESP_OK) return err;

    const argus_nvs_driver_t *drv = get_driver();
    uint8_t inactive_slot_index = (s_active_slot_index == 0) ? 1 : 0;

    argus_cfg_slot_t slot = {0};
    slot.schema_version = ARGUS_CONFIG_SCHEMA_VERSION;
    slot.config_generation = (s_active_generation == 0xFFFFFFFFU) ? 1 : s_active_generation + 1;
    slot.payload_length = sizeof(argus_config_payload_t);
    memcpy(&slot.payload, in_cfg, sizeof(argus_config_payload_t));
    slot.crc32 = argus_nvs_config_calc_crc32(&slot.payload);
    slot.valid_marker = ARGUS_CONFIG_VALID_MARKER;

    // Step 1: Write and commit inactive slot
    err = drv->write_slot(inactive_slot_index, &slot);
    if (err != ESP_OK) return err;

    // Step 2: Read back and verify
    argus_cfg_slot_t readback = {0};
    err = drv->read_slot(inactive_slot_index, &readback);
    if (err != ESP_OK || !is_slot_valid(&readback)) {
        return ESP_ERR_INVALID_STATE;
    }

    // Step 3: Write active selector
    err = drv->write_selector(inactive_slot_index);
    if (err != ESP_OK) return err;

    // Update active in-memory cache
    s_active_slot_index = inactive_slot_index;
    s_active_generation = slot.config_generation;
    memcpy(&s_active_config, in_cfg, sizeof(argus_config_payload_t));
    s_has_valid_config = true;

    return ESP_OK;
}

esp_err_t argus_nvs_config_factory_reset(void)
{
    const argus_nvs_driver_t *drv = get_driver();
    drv->write_reset_pending(true);
    drv->erase_all();
    drv->write_reset_pending(false);

    memset(&s_active_config, 0, sizeof(argus_config_payload_t));
    snprintf(s_active_config.client_id, sizeof(s_active_config.client_id), "default_client");
    snprintf(s_active_config.unit_id, sizeof(s_active_config.unit_id), "unit_01");
    snprintf(s_active_config.device_name, sizeof(s_active_config.device_name), "Argus Peristaltic Pump V2");
    s_active_generation = 0;
    s_has_valid_config = false;

    return ESP_OK;
}

void argus_nvs_config_mask(const argus_config_payload_t *in_cfg, argus_config_payload_t *out_masked)
{
    if (!in_cfg || !out_masked) return;
    memcpy(out_masked, in_cfg, sizeof(argus_config_payload_t));
    if (strlen(out_masked->sta_pass) > 0) {
        snprintf(out_masked->sta_pass, sizeof(out_masked->sta_pass), "%s", ARGUS_CONFIG_MASK_STRING);
    }
}
