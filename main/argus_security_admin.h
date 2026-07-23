#ifndef ARGUS_SECURITY_ADMIN_H
#define ARGUS_SECURITY_ADMIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_authorization.h"
#include "argus_security_directory.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool argus_security_admin_removal_would_lock_out(
    const argus_security_directory_payload_t *payload,
    size_t target_index);

esp_err_t argus_security_admin_create_human(
    const argus_principal_t *actor,
    const char *identifier,
    const char *canonical_login,
    const char *display_name,
    const char *scope,
    argus_security_level_t level,
    argus_permission_set_t direct_permissions,
    const uint8_t *password,
    size_t password_len);

esp_err_t argus_security_admin_set_enabled(
    const argus_principal_t *actor,
    const char *identifier,
    bool enabled);

esp_err_t argus_security_admin_replace_password(
    const argus_principal_t *actor,
    const char *identifier,
    const uint8_t *password,
    size_t password_len);

esp_err_t argus_security_admin_replace_own_password(
    const argus_principal_t *actor,
    const uint8_t *password,
    size_t password_len);

esp_err_t argus_security_admin_update_display_name(
    const argus_principal_t *actor,
    const char *identifier,
    const char *display_name);

esp_err_t argus_security_admin_assign_custom_roles(
    const argus_principal_t *actor,
    const char *identifier,
    uint16_t custom_role_mask);

esp_err_t argus_security_admin_delete_human(
    const argus_principal_t *actor,
    const char *identifier);

esp_err_t argus_security_admin_create_role(
    const argus_principal_t *actor,
    const char *identifier,
    argus_security_level_t level,
    argus_permission_set_t permissions,
    argus_permission_set_t delegable_permissions);

esp_err_t argus_security_admin_delete_role(
    const argus_principal_t *actor,
    const char *identifier);

#ifdef __cplusplus
}
#endif

#endif
