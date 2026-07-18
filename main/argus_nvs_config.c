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
#include "esp_log.h"

static const char *TAG = "argus_nvs_cfg";

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
// Default production NVS storage driver implementations
static esp_err_t prod_read_slot(void *ctx, uint8_t slot_index, argus_cfg_slot_t *out_slot)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    const char *key = (slot_index == 0) ? ARGUS_NVS_KEY_SLOT_0 : ARGUS_NVS_KEY_SLOT_1;
    size_t len = sizeof(argus_cfg_slot_t);
    err = nvs_get_blob(handle, key, out_slot, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t prod_write_slot(void *ctx, uint8_t slot_index, const argus_cfg_slot_t *in_slot)
{
    (void)ctx;
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

static esp_err_t prod_read_selector(void *ctx, uint8_t *out_selector)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_u8(handle, ARGUS_NVS_KEY_SELECTOR, out_selector);
    nvs_close(handle);
    return err;
}

static esp_err_t prod_write_selector(void *ctx, uint8_t selector)
{
    (void)ctx;
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

static esp_err_t prod_read_reset_pending(void *ctx, bool *out_pending)
{
    (void)ctx;
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

static esp_err_t prod_write_reset_pending(void *ctx, bool pending)
{
    (void)ctx;
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

static esp_err_t prod_erase_all(void *ctx)
{
    (void)ctx;
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
    .erase_all = prod_erase_all,
    .ctx = NULL
};

esp_err_t argus_nvs_core_init(argus_nvs_core_t *core, const argus_nvs_driver_t *driver)
{
    if (!core || !driver) return ESP_ERR_INVALID_ARG;
    memset(core, 0, sizeof(argus_nvs_core_t));
    core->driver = driver;

    argus_cfg_slot_t slot_a = {0}, slot_b = {0};
    esp_err_t err_a = driver->read_slot(driver->ctx, 0, &slot_a);
    bool valid_a = (err_a == ESP_OK) && is_slot_valid(&slot_a);

    esp_err_t err_b = driver->read_slot(driver->ctx, 1, &slot_b);
    bool valid_b = (err_b == ESP_OK) && is_slot_valid(&slot_b);

    uint8_t selector = 0xFF;
    driver->read_selector(driver->ctx, &selector);

    if (valid_a && valid_b) {
        if (selector == 0) {
            core->active_slot_index = 0;
            memcpy(&core->active_config, &slot_a.payload, sizeof(argus_config_payload_t));
            core->active_generation = slot_a.config_generation;
            core->has_valid_config = true;
        } else if (selector == 1) {
            core->active_slot_index = 1;
            memcpy(&core->active_config, &slot_b.payload, sizeof(argus_config_payload_t));
            core->active_generation = slot_b.config_generation;
            core->has_valid_config = true;
        } else {
            if (argus_nvs_config_gen_is_newer(slot_b.config_generation, slot_a.config_generation)) {
                core->active_slot_index = 1;
                memcpy(&core->active_config, &slot_b.payload, sizeof(argus_config_payload_t));
                core->active_generation = slot_b.config_generation;
            } else {
                core->active_slot_index = 0;
                memcpy(&core->active_config, &slot_a.payload, sizeof(argus_config_payload_t));
                core->active_generation = slot_a.config_generation;
            }
            core->has_valid_config = true;
        }
    } else if (valid_a) {
        core->active_slot_index = 0;
        memcpy(&core->active_config, &slot_a.payload, sizeof(argus_config_payload_t));
        core->active_generation = slot_a.config_generation;
        core->has_valid_config = true;
    } else if (valid_b) {
        core->active_slot_index = 1;
        memcpy(&core->active_config, &slot_b.payload, sizeof(argus_config_payload_t));
        core->active_generation = slot_b.config_generation;
        core->has_valid_config = true;
    } else {
        memset(&core->active_config, 0, sizeof(argus_config_payload_t));
        core->active_generation = 0;
        core->has_valid_config = false;
    }

    core->initialized = true;
    return ESP_OK;
}

esp_err_t argus_nvs_core_get(const argus_nvs_core_t *core, argus_config_payload_t *out_cfg)
{
    if (!core || !out_cfg) return ESP_ERR_INVALID_ARG;
    if (!core->has_valid_config) return ESP_ERR_NOT_FOUND;
    memcpy(out_cfg, &core->active_config, sizeof(argus_config_payload_t));
    return ESP_OK;
}

esp_err_t argus_nvs_core_commit(argus_nvs_core_t *core, const argus_config_payload_t *in_cfg)
{
    if (!core || !in_cfg || !core->driver) return ESP_ERR_INVALID_ARG;

    if (strcmp(in_cfg->sta_pass, ARGUS_CONFIG_MASK_STRING) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t val_err = argus_nvs_config_validate(in_cfg);
    if (val_err != ESP_OK) return val_err;

    uint8_t inactive_slot = (core->active_slot_index == 0) ? 1 : 0;
    uint32_t next_gen = core->active_generation + 1;

    argus_cfg_slot_t slot = {
        .schema_version = ARGUS_CONFIG_SCHEMA_VERSION,
        .config_generation = next_gen,
        .payload_length = sizeof(argus_config_payload_t),
        .crc32 = argus_nvs_config_calc_crc32(in_cfg),
        .valid_marker = ARGUS_CONFIG_VALID_MARKER
    };
    memcpy(&slot.payload, in_cfg, sizeof(argus_config_payload_t));

    esp_err_t write_err = core->driver->write_slot(core->driver->ctx, inactive_slot, &slot);
    if (write_err != ESP_OK) return write_err;

    argus_cfg_slot_t verify_slot = {0};
    esp_err_t readback_err = core->driver->read_slot(core->driver->ctx, inactive_slot, &verify_slot);
    if (readback_err != ESP_OK || !is_slot_valid(&verify_slot) ||
        memcmp(&verify_slot.payload, in_cfg, sizeof(argus_config_payload_t)) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t sel_err = core->driver->write_selector(core->driver->ctx, inactive_slot);
    if (sel_err != ESP_OK) return sel_err;

    argus_cfg_slot_t final_slot = {0};
    esp_err_t final_read_err = core->driver->read_slot(core->driver->ctx, inactive_slot, &final_slot);
    if (final_read_err != ESP_OK || !is_slot_valid(&final_slot)) {
        return ESP_ERR_INVALID_STATE;
    }

    core->active_slot_index = inactive_slot;
    core->active_generation = next_gen;
    memcpy(&core->active_config, &final_slot.payload, sizeof(argus_config_payload_t));
    core->has_valid_config = true;
    return ESP_OK;
}

static const argus_nvs_driver_t *get_driver(void)
{
    return s_custom_driver ? s_custom_driver : &s_prod_driver;
}

esp_err_t argus_nvs_config_init(const argus_nvs_driver_t *driver)
{
    s_custom_driver = driver;

    esp_err_t flash_err = ESP_OK;
    if (!s_custom_driver) {
        flash_err = nvs_flash_init();
        if (flash_err == ESP_ERR_NVS_NO_FREE_PAGES || flash_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            flash_err = nvs_flash_init();
        }
        ESP_LOGI(TAG, "nvs_flash_init() result: %s (%d)", esp_err_to_name(flash_err), flash_err);
    }

    const argus_nvs_driver_t *drv = get_driver();

    // Check for power-interrupted factory reset recovery
    bool reset_pending = false;
    drv->read_reset_pending(drv->ctx, &reset_pending);
    if (reset_pending) {
        ESP_LOGW(TAG, "Factory reset pending marker detected. Recovering...");
        drv->erase_all(drv->ctx);
        drv->write_reset_pending(drv->ctx, false);
    }

    // Load dual slots
    argus_cfg_slot_t slot_a = {0}, slot_b = {0};
    esp_err_t err_a = drv->read_slot(drv->ctx, 0, &slot_a);
    bool valid_a = (err_a == ESP_OK) && is_slot_valid(&slot_a);

    esp_err_t err_b = drv->read_slot(drv->ctx, 1, &slot_b);
    bool valid_b = (err_b == ESP_OK) && is_slot_valid(&slot_b);

    uint8_t selector = 0xFF;
    esp_err_t err_sel = drv->read_selector(drv->ctx, &selector);

    ESP_LOGI(TAG, "Slot A (0): read=%s len=%u schema=%u gen=%lu marker=0x%08X valid=%s",
             esp_err_to_name(err_a), (unsigned)slot_a.payload_length, (unsigned)slot_a.schema_version,
             (unsigned long)slot_a.config_generation, (unsigned)slot_a.valid_marker, valid_a ? "YES" : "NO");
    ESP_LOGI(TAG, "Slot B (1): read=%s len=%u schema=%u gen=%lu marker=0x%08X valid=%s",
             esp_err_to_name(err_b), (unsigned)slot_b.payload_length, (unsigned)slot_b.schema_version,
             (unsigned long)slot_b.config_generation, (unsigned)slot_b.valid_marker, valid_b ? "YES" : "NO");
    ESP_LOGI(TAG, "Stored Selector: read=%s active_slot=0x%02X", esp_err_to_name(err_sel), selector);

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
            drv->write_selector(drv->ctx, s_active_slot_index);
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

    bool is_comm = argus_nvs_config_is_commissioned(&s_active_config);
    ESP_LOGI(TAG, "Selected LKG Slot: %u (gen %lu), Commissioned: %s (%s)",
             s_active_slot_index, (unsigned long)s_active_generation,
             is_comm ? "YES" : "NO", argus_nvs_config_get_uncommissioned_reason(&s_active_config));

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
    if (ssid_len > 0) {
        if (pass_len < ARGUS_CFG_STA_PASS_MIN || pass_len > ARGUS_CFG_STA_PASS_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

const char *argus_nvs_config_get_uncommissioned_reason(const argus_config_payload_t *cfg)
{
    if (!cfg) return "Null payload pointer";
    if (!argus_identity_validate_client_id(cfg->client_id)) return "Invalid Client ID";
    if (!argus_identity_validate_unit_id(cfg->unit_id)) return "Invalid Unit ID";
    if (!argus_identity_validate_device_name(cfg->device_name)) return "Invalid Device Name";
    if (strlen(cfg->sta_ssid) == 0) return "STA SSID is empty";
    size_t pass_len = strlen(cfg->sta_pass);
    if (pass_len < ARGUS_CFG_STA_PASS_MIN) return "STA Pass length < 8 chars";
    if (pass_len > ARGUS_CFG_STA_PASS_MAX) return "STA Pass length > 63 chars";
    return "Valid / Commissioned";
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

    if (strcmp(in_cfg->sta_pass, ARGUS_CONFIG_MASK_STRING) == 0) {
        ESP_LOGE(TAG, "Commit rejected: Mask string submitted as password");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = argus_nvs_config_validate(in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Commit rejected: Staged payload failed Schema V1 validation (%s)", esp_err_to_name(err));
        return err;
    }

    const argus_nvs_driver_t *drv = get_driver();
    uint8_t inactive_slot_index = (s_active_slot_index == 0) ? 1 : 0;

    argus_cfg_slot_t slot;
    memset(&slot, 0, sizeof(argus_cfg_slot_t));
    slot.schema_version = ARGUS_CONFIG_SCHEMA_VERSION;
    slot.config_generation = (s_active_generation == 0xFFFFFFFFU) ? 1 : s_active_generation + 1;
    slot.payload_length = sizeof(argus_config_payload_t);
    memcpy(&slot.payload, in_cfg, sizeof(argus_config_payload_t));
    slot.crc32 = argus_nvs_config_calc_crc32(&slot.payload);
    slot.valid_marker = ARGUS_CONFIG_VALID_MARKER;

    // Step 1: Write inactive slot
    ESP_LOGI(TAG, "Commit Step 1-3/12: Writing inactive slot %u (gen %lu)...", inactive_slot_index, (unsigned long)slot.config_generation);
    err = drv->write_slot(drv->ctx, inactive_slot_index, &slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed at Step 1 (slot write error): %s", esp_err_to_name(err));
        return err;
    }

    // Step 4-6: Production readback verification
    ESP_LOGI(TAG, "Commit Step 4-6/12: Reopening namespace & verifying production readback...");
    argus_cfg_slot_t readback;
    memset(&readback, 0, sizeof(argus_cfg_slot_t));
    err = drv->read_slot(drv->ctx, inactive_slot_index, &readback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed at Step 5 (readback read error): %s", esp_err_to_name(err));
        return err;
    }
    if (!is_slot_valid(&readback)) {
        ESP_LOGE(TAG, "NVS commit failed at Step 6 (readback slot validation failed)");
        return ESP_ERR_INVALID_STATE;
    }
    if (memcmp(&readback.payload, in_cfg, sizeof(argus_config_payload_t)) != 0) {
        ESP_LOGE(TAG, "NVS commit failed at Step 6 (readback payload byte mismatch)");
        return ESP_ERR_INVALID_STATE;
    }

    // Step 7-8: Commit active selector
    ESP_LOGI(TAG, "Commit Step 7-8/12: Committing active selector to %u...", inactive_slot_index);
    err = drv->write_selector(drv->ctx, inactive_slot_index);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed at Step 7 (selector write error): %s", esp_err_to_name(err));
        return err;
    }

    // Step 9-11: Production LKG loader verification
    ESP_LOGI(TAG, "Commit Step 9-11/12: Verifying LKG loader selection...");
    uint8_t read_sel = 0xFF;
    err = drv->read_selector(drv->ctx, &read_sel);
    if (err != ESP_OK || read_sel != inactive_slot_index) {
        ESP_LOGE(TAG, "NVS commit failed at Step 10 (selector readback verification failed)");
        return ESP_ERR_INVALID_STATE;
    }

    if (!argus_nvs_config_is_commissioned(in_cfg)) {
        ESP_LOGE(TAG, "NVS commit failed at Step 11 (is_commissioned evaluated false)");
        return ESP_ERR_INVALID_STATE;
    }

    // Update in-memory active state only after 100% verification
    s_active_slot_index = inactive_slot_index;
    s_active_generation = slot.config_generation;
    memcpy(&s_active_config, in_cfg, sizeof(argus_config_payload_t));
    s_has_valid_config = true;

    ESP_LOGI(TAG, "Commit Step 12/12: NVS commit & production readback verification PASSED cleanly.");
    return ESP_OK;
}

esp_err_t argus_nvs_config_factory_reset(void)
{
    const argus_nvs_driver_t *drv = get_driver();
    drv->write_reset_pending(drv->ctx, true);
    drv->erase_all(drv->ctx);
    drv->write_reset_pending(drv->ctx, false);

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

esp_err_t argus_nvs_config_get_observation_snapshot(argus_nvs_observation_t *out_obs)
{
    if (!out_obs) return ESP_ERR_INVALID_ARG;

    const argus_nvs_driver_t *drv = get_driver();
    esp_err_t first_unexpected = ESP_OK;

    // Read selector
    memset(out_obs, 0, sizeof(*out_obs));
    out_obs->selector_status = drv->read_selector(drv->ctx, &out_obs->selector);
    if (out_obs->selector_status == ESP_OK) {
        out_obs->selector_present = true;
    } else if (out_obs->selector_status == ESP_ERR_NOT_FOUND) {
        out_obs->selector_present = false;
    } else {
        out_obs->selector_present = false;
        if (first_unexpected == ESP_OK) first_unexpected = out_obs->selector_status;
    }

    // Read slot A
    out_obs->slot_a_status = drv->read_slot(drv->ctx, 0, &out_obs->slot_a);
    if (out_obs->slot_a_status == ESP_OK) {
        out_obs->slot_a_present = true;
        out_obs->slot_a_valid = is_slot_valid(&out_obs->slot_a);
    } else if (out_obs->slot_a_status == ESP_ERR_NOT_FOUND) {
        out_obs->slot_a_present = false;
        out_obs->slot_a_valid = false;
    } else {
        out_obs->slot_a_present = false;
        out_obs->slot_a_valid = false;
        if (first_unexpected == ESP_OK) first_unexpected = out_obs->slot_a_status;
    }

    // Read slot B
    out_obs->slot_b_status = drv->read_slot(drv->ctx, 1, &out_obs->slot_b);
    if (out_obs->slot_b_status == ESP_OK) {
        out_obs->slot_b_present = true;
        out_obs->slot_b_valid = is_slot_valid(&out_obs->slot_b);
    } else if (out_obs->slot_b_status == ESP_ERR_NOT_FOUND) {
        out_obs->slot_b_present = false;
        out_obs->slot_b_valid = false;
    } else {
        out_obs->slot_b_present = false;
        out_obs->slot_b_valid = false;
        if (first_unexpected == ESP_OK) first_unexpected = out_obs->slot_b_status;
    }

    return first_unexpected;
}
