/**
 * @file argus_nvs_config.h
 * @brief Power-Loss-Safe Dual-Slot NVS Configuration Manager for Argus Pump Controller V2
 */

#ifndef ARGUS_NVS_CONFIG_H
#define ARGUS_NVS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_CONFIG_SCHEMA_VERSION     2
#define ARGUS_CONFIG_SCHEMA_V1          1
#define ARGUS_CONFIG_PAYLOAD_V1_SIZE    228  /* packed size without provisioned_flags */
#define ARGUS_CONFIG_VALID_MARKER       0xA5A55A5AU
#define ARGUS_CONFIG_MASK_STRING        "********"

#define ARGUS_CFG_CLIENT_ID_MAX         32
#define ARGUS_CFG_UNIT_ID_MAX           32
#define ARGUS_CFG_DEV_NAME_MAX          64
#define ARGUS_CFG_STA_SSID_MAX          32
#define ARGUS_CFG_STA_PASS_MIN          8
#define ARGUS_CFG_STA_PASS_MAX          63

#define ARGUS_CFG_PROVISIONED_IDENTITY  0x01  /**< Bit 0: identity is locked after initial provisioning */

typedef struct __attribute__((packed)) {
    char client_id[ARGUS_CFG_CLIENT_ID_MAX + 1];   /**< 32 chars + null */
    char unit_id[ARGUS_CFG_UNIT_ID_MAX + 1];       /**< 32 chars + null */
    char device_name[ARGUS_CFG_DEV_NAME_MAX + 1];  /**< 64 chars + null */
    char sta_ssid[ARGUS_CFG_STA_SSID_MAX + 1];     /**< 32 bytes + null */
    char sta_pass[ARGUS_CFG_STA_PASS_MAX + 1];     /**< 63 chars + null */
    uint8_t provisioned_flags;                     /**< Bitfield: ARGUS_CFG_PROVISIONED_IDENTITY */
} argus_config_payload_t;

typedef struct __attribute__((packed)) {
    uint16_t schema_version;
    uint32_t config_generation;
    uint16_t payload_length;
    uint32_t crc32;
    uint32_t valid_marker;
    argus_config_payload_t payload;
} argus_cfg_slot_t;

/**
 * @brief Abstract storage driver interface for dependency injection (pure unit testing).
 */
typedef struct {
    esp_err_t (*read_slot)(void *ctx, uint8_t slot_index, argus_cfg_slot_t *out_slot);
    esp_err_t (*write_slot)(void *ctx, uint8_t slot_index, const argus_cfg_slot_t *in_slot);
    esp_err_t (*read_selector)(void *ctx, uint8_t *out_selector);
    esp_err_t (*write_selector)(void *ctx, uint8_t selector);
    esp_err_t (*read_reset_pending)(void *ctx, bool *out_pending);
    esp_err_t (*write_reset_pending)(void *ctx, bool pending);
    esp_err_t (*read_provisioned_hwm)(void *ctx, uint8_t *out_flags);   /**< Monotonic provisioning marker */
    esp_err_t (*write_provisioned_hwm)(void *ctx, uint8_t flags);       /**< Write-once per provisioning event */
    esp_err_t (*erase_all)(void *ctx);
    void *ctx;
} argus_nvs_driver_t;

typedef struct {
    argus_config_payload_t active_config;
    uint32_t active_generation;
    uint8_t active_slot_index;
    bool has_valid_config;
    bool initialized;
    const argus_nvs_driver_t *driver;
} argus_nvs_core_t;

/**
 * @brief Pure dual-slot NVS core functions operating on caller-provided instances (for unit testing and core evaluation).
 */
esp_err_t argus_nvs_core_init(argus_nvs_core_t *core, const argus_nvs_driver_t *driver);
esp_err_t argus_nvs_core_get(const argus_nvs_core_t *core, argus_config_payload_t *out_cfg);
esp_err_t argus_nvs_core_commit(argus_nvs_core_t *core, const argus_config_payload_t *in_cfg);

/**
 * @brief Pure boot-recovery transaction: check pending marker, re-erase if set, clear marker.
 * @param drv  Injected storage driver.
 * @return ESP_OK if no recovery needed or recovery completed successfully.
 */
esp_err_t argus_nvs_core_recovery_check(const argus_nvs_driver_t *drv);

/**
 * @brief Pure factory-reset transaction: mark pending, erase, clear, reinit core.
 *
 * Production policy: if clearing the pending marker fails after a successful
 * erase, the core is still reinitialized to match the erased storage.
 *
 * @param core  Caller-owned core state to reinitialize.
 * @param drv   Injected storage driver.
 * @return ESP_OK on full success; exact originating error otherwise.
 */
esp_err_t argus_nvs_core_factory_reset(argus_nvs_core_t *core, const argus_nvs_driver_t *drv);

/**
 * @brief Initialize NVS configuration manager using production or injected driver.
 * @param driver Optional mock driver for unit testing; pass NULL for production ESP-IDF NVS.
 * @return ESP_OK on success.
 */
esp_err_t argus_nvs_config_init(const argus_nvs_driver_t *driver);

/**
 * @brief Load current effective configuration payload (persisted or defaults).
 * @param[out] out_cfg Destination payload struct (populated with defaults if uncommissioned).
 * @param[out] out_has_persisted_config Set to true if a valid LKG slot exists in NVS.
 * @return ESP_OK if effective configuration is available; error if initialization or core access genuinely failed.
 */
esp_err_t argus_nvs_config_get_effective(argus_config_payload_t *out_cfg, bool *out_has_persisted_config);

/**
 * @brief Stage and commit a new configuration payload via power-loss-safe dual-slot sequence.
 * @param in_cfg Staged configuration payload.
 * @return ESP_OK on success, error code if validation or write fails.
 */
esp_err_t argus_nvs_config_commit(const argus_config_payload_t *in_cfg);

/**
 * @brief Evaluate whether a given configuration payload is fully commissioned.
 * @param cfg Configuration payload to check.
 * @return true if valid schema, non-empty identity, and valid STA WPA2 credentials (8..63 chars).
 */
bool argus_nvs_config_is_commissioned(const argus_config_payload_t *cfg);

/**
 * @brief Get sanitized non-secret reason why a payload evaluated uncommissioned.
 */
const char *argus_nvs_config_get_uncommissioned_reason(const argus_config_payload_t *cfg);

/**
 * @brief Validate staged configuration payload fields before write.
 * @param cfg Configuration payload to validate.
 * @return ESP_OK if valid, ESP_ERR_INVALID_ARG if any field violates schema rules.
 */
esp_err_t argus_nvs_config_validate(const argus_config_payload_t *cfg);

/**
 * @brief Execute power-loss-recoverable factory reset.
 * @return ESP_OK on success.
 */
esp_err_t argus_nvs_config_factory_reset(void);

/**
 * @brief Return whether an NVS namespace belongs to configuration-reset scope.
 *
 * The durable reset marker and portal credential namespaces are deliberately
 * outside this scope.
 */
bool argus_nvs_config_namespace_in_factory_reset_scope(const char *namespace_name);

/**
 * @brief Export configuration payload with sensitive fields (passwords) masked.
 * @param in_cfg Source configuration.
 * @param[out] out_masked Destination masked configuration.
 */
void argus_nvs_config_mask(const argus_config_payload_t *in_cfg, argus_config_payload_t *out_masked);

/**
 * @brief Calculate CRC32 checksum over payload bytes.
 */
uint32_t argus_nvs_config_calc_crc32(const argus_config_payload_t *payload);

/**
 * @brief Wrap-safe generation comparison using serial number arithmetic.
 * @return true if gen_a is strictly newer than gen_b.
 */
bool argus_nvs_config_gen_is_newer(uint32_t gen_a, uint32_t gen_b);

/**
 * @brief NVS observation result with per-slot error differentiation.
 *
 * - status == ESP_OK: data was read successfully (present = true)
 * - status == ESP_ERR_NOT_FOUND: key does not exist (present = false, valid = false)
 * - status == other: driver error (present = false, valid = false)
 * - valid: slot present AND valid_marker == ARGUS_CONFIG_VALID_MARKER
 */
typedef struct {
    esp_err_t selector_status;
    bool      selector_present;
    uint8_t   selector;

    esp_err_t slot_a_status;
    bool      slot_a_present;
    bool      slot_a_valid;
    argus_cfg_slot_t slot_a;

    esp_err_t slot_b_status;
    bool      slot_b_present;
    bool      slot_b_valid;
    argus_cfg_slot_t slot_b;
} argus_nvs_observation_t;

/**
 * @brief Read-only observation of NVS selector and dual-slot metadata without state mutation.
 *
 * Missing NVS configuration is valid for an uncommissioned device.
 *
 * @param[out] out_obs Observation result.
 * @return ESP_OK if all reads returned ESP_OK or ESP_ERR_NOT_FOUND.
 *         Returns the first unexpected error otherwise.
 */
esp_err_t argus_nvs_config_get_observation_snapshot(argus_nvs_observation_t *out_obs);

/**
 * @brief Pure helper for read-only observation.
 */
esp_err_t argus_nvs_core_get_observation_snapshot(const argus_nvs_driver_t *drv, argus_nvs_observation_t *out_obs);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_NVS_CONFIG_H */
