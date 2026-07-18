/**
 * @file argus_authority_mgr.h
 * @brief Exclusive Control Authority & Owner Manager for Argus Pump Controller V2
 */

#ifndef ARGUS_AUTHORITY_MGR_H
#define ARGUS_AUTHORITY_MGR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "argus_cmd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARGUS_AUTHORITY_NONE = 0,
    ARGUS_AUTHORITY_SUPERVISORY,
    ARGUS_AUTHORITY_SERVICE_TRANSITION,
    ARGUS_AUTHORITY_LOCAL_SERVICE
} argus_control_authority_t;

typedef enum {
    ARGUS_AUTH_OWNER_NONE = 0,
    ARGUS_AUTH_OWNER_MQTT,
    ARGUS_AUTH_OWNER_BROWSER,
    ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI
} argus_authority_owner_t;

typedef struct {
    argus_control_authority_t mode;
    argus_authority_owner_t owner;
    uint32_t generation;
} argus_authority_snapshot_t;

/**
 * @brief Initialize control authority manager.
 * @return ESP_OK on success.
 */
esp_err_t argus_authority_mgr_init(void);

/**
 * @brief Get coherent thread-safe snapshot of authority mode, owner, and generation.
 * @param[out] out_snap Pointer to destination snapshot struct.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_snap is NULL.
 */
esp_err_t argus_authority_mgr_get_snapshot(argus_authority_snapshot_t *out_snap);

/**
 * @brief Transition control authority mode and owner (increments authority generation).
 * @param new_mode Proposed authority mode.
 * @param new_owner Proposed authority owner.
 * @return ESP_OK on success.
 */
esp_err_t argus_authority_mgr_set_mode(argus_control_authority_t new_mode, argus_authority_owner_t new_owner);

/**
 * @brief Request controlled transition into LOCAL_SERVICE mode for specified owner.
 * @param requested_owner Target owner (ARGUS_AUTH_OWNER_BROWSER or ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid owner requested.
 */
esp_err_t argus_authority_request_service(argus_authority_owner_t requested_owner);

/**
 * @brief Request controlled service exit and reboot.
 * @return ESP_OK on success.
 */
esp_err_t argus_authority_request_exit(void);

/**
 * @brief Validate whether a command source and type are permitted under the given snapshot.
 * @param snap Snapshot to check.
 * @param source Incoming command source.
 * @param cmd_type Incoming command type.
 * @return true if permitted, false if rejected.
 */
bool argus_authority_validate_permission(const argus_authority_snapshot_t *snap,
                                         argus_cmd_source_t source,
                                         argus_cmd_type_t cmd_type);

/**
 * @brief Get human-readable string name for authority mode.
 */
const char *argus_authority_mgr_get_mode_name(argus_control_authority_t mode);

/**
 * @brief Get human-readable string name for authority owner.
 */
const char *argus_authority_mgr_get_owner_name(argus_authority_owner_t owner);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_AUTHORITY_MGR_H */
