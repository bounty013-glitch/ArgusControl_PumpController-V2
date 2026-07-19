/**
 * @file argus_config_overlay.h
 * @brief Pure Configuration Overlay Seam for Argus Pump Controller V2
 *
 * Extracts the configuration overlay logic from the HTTP handler into
 * a pure, testable function. The HTTP handler parses JSON into
 * argus_config_fields_t, then delegates to argus_config_overlay_apply().
 *
 * All overlay decisions are made by this module. The HTTP handler only
 * handles transport (JSON parsing, HTTP response codes) and NVS commit.
 */

#ifndef ARGUS_CONFIG_OVERLAY_H
#define ARGUS_CONFIG_OVERLAY_H

#include <stdbool.h>
#include "argus_nvs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration update scope.
 */
typedef enum {
    ARGUS_CONFIG_SCOPE_IDENTITY = 0,
    ARGUS_CONFIG_SCOPE_WIFI,
    ARGUS_CONFIG_SCOPE_INVALID    /**< Sentinel for unknown/malformed scope */
} argus_config_scope_t;

/**
 * @brief Parsed field presence and values from HTTP request body.
 *
 * Populated by the HTTP handler's JSON parser before calling the overlay.
 * Fields are only read if their corresponding has_* flag is true.
 */
typedef struct {
    /* Identity fields */
    bool has_client_id;
    bool has_unit_id;
    bool has_device_name;
    char client_id[ARGUS_CFG_CLIENT_ID_MAX + 1];
    char unit_id[ARGUS_CFG_UNIT_ID_MAX + 1];
    char device_name[ARGUS_CFG_DEV_NAME_MAX + 1];

    /* WiFi fields */
    bool has_sta_ssid;
    bool has_sta_pass;
    char sta_ssid[ARGUS_CFG_STA_SSID_MAX + 1];
    char sta_pass[ARGUS_CFG_STA_PASS_MAX + 1];
} argus_config_fields_t;

/**
 * @brief Result of a configuration overlay operation.
 */
typedef struct {
    bool success;                /**< true if overlay was applied successfully */
    const char *error_code;      /**< Machine-readable error (static string) */
    const char *error_message;   /**< Human-readable error (static string) */
} argus_config_overlay_result_t;

/**
 * @brief Apply a scoped configuration overlay to the current configuration.
 *
 * Pure function — no I/O, no NVS, no HTTP. All inputs are caller-provided.
 *
 * Identity scope:
 * - Rejects if current config has PROVISIONED_IDENTITY flag set.
 * - Requires all three identity fields present.
 * - Validates each field (client_id, unit_id, device_name).
 * - Sets PROVISIONED_IDENTITY flag atomically in output.
 * - Preserves WiFi fields from current config.
 *
 * WiFi scope:
 * - Requires sta_ssid field present.
 * - Empty SSID clears both SSID and password.
 * - Non-empty SSID with new password: replaces password.
 * - Same SSID without password: preserves existing password.
 * - New SSID without password: rejected.
 * - Mask string password: rejected.
 * - Preserves identity fields and provisioned_flags from current config.
 *
 * @param current   Current configuration from NVS (read before overlay).
 * @param scope     Update scope (IDENTITY or WIFI).
 * @param fields    Parsed field presence and values.
 * @param out_cfg   Output configuration (only written on success).
 * @return Result with error details on failure.
 */
argus_config_overlay_result_t argus_config_overlay_apply(
    const argus_config_payload_t *current,
    argus_config_scope_t scope,
    const argus_config_fields_t *fields,
    argus_config_payload_t *out_cfg);

/**
 * @brief Parse scope string into enum.
 * @param scope_str Scope string from JSON ("identity" or "wifi").
 * @return Parsed scope enum, or ARGUS_CONFIG_SCOPE_INVALID.
 */
argus_config_scope_t argus_config_overlay_parse_scope(const char *scope_str);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_CONFIG_OVERLAY_H */
