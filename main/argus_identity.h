/**
 * @file argus_identity.h
 * @brief Persistent Device Identity and Metadata Module for Argus Pump Controller V2
 */

#ifndef ARGUS_IDENTITY_H
#define ARGUS_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_IDENTITY_UID_LEN          32
#define ARGUS_IDENTITY_CLIENT_ID_MAX    32
#define ARGUS_IDENTITY_UNIT_ID_MAX      32
#define ARGUS_IDENTITY_DEV_NAME_MAX     64
#define ARGUS_IDENTITY_MODEL_MAX        32
#define ARGUS_IDENTITY_FW_VER_MAX       64
#define ARGUS_IDENTITY_SERVICE_SSID_MAX 32

typedef struct {
    char mac_uid[ARGUS_IDENTITY_UID_LEN + 1];           /**< Derived immutable hardware UID (e.g. ESP32S3-A1B2C3D4E5F6) */
    char client_id[ARGUS_IDENTITY_CLIENT_ID_MAX + 1];   /**< Configurable client namespace identifier */
    char unit_id[ARGUS_IDENTITY_UNIT_ID_MAX + 1];       /**< Configurable unit identifier */
    char device_name[ARGUS_IDENTITY_DEV_NAME_MAX + 1]; /**< Human-readable device display name */
    char device_model[ARGUS_IDENTITY_MODEL_MAX + 1];   /**< Read-only model string (ARGUS-PUMP-V2) */
    char fw_version[ARGUS_IDENTITY_FW_VER_MAX + 1];     /**< Read-only application version descriptor */
    char service_ssid[ARGUS_IDENTITY_SERVICE_SSID_MAX + 1]; /**< Derived service AP SSID (Argus-Service-AABBCC) */
} argus_identity_t;

/**
 * @brief Initialize device identity subsystem and read hardware eFuse MAC address.
 * @return ESP_OK on success.
 */
esp_err_t argus_identity_init(void);

/**
 * @brief Get coherent snapshot of active device identity.
 * @param[out] out_id Pointer to destination identity structure.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_id is NULL.
 */
esp_err_t argus_identity_get(argus_identity_t *out_id);

/**
 * @brief Validate a proposed client_id string.
 * @param client_id String to validate (must be 1..32 chars, alphanumeric, '-' or '_').
 * @return true if valid, false otherwise.
 */
bool argus_identity_validate_client_id(const char *client_id);

/**
 * @brief Validate a proposed unit_id string.
 * @param unit_id String to validate (must be 1..32 chars, alphanumeric, '-' or '_').
 * @return true if valid, false otherwise.
 */
bool argus_identity_validate_unit_id(const char *unit_id);

/**
 * @brief Validate a proposed device_name string.
 * @param device_name String to validate (must be 1..64 printable ASCII chars).
 * @return true if valid, false otherwise.
 */
bool argus_identity_validate_device_name(const char *device_name);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_IDENTITY_H */
