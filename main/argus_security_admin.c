#include "argus_security_admin.h"

#include <stdlib.h>
#include <string.h>

#include "argus_auth_service.h"
#include "argus_password_verifier.h"
#include "argus_session_manager.h"

static esp_err_t directory_snapshot_alloc(
    argus_security_directory_snapshot_t **out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = calloc(1U, sizeof(**out));
    if (*out == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = argus_security_directory_get_snapshot(*out);
    if (err != ESP_OK) {
        argus_password_zeroize(*out, sizeof(**out));
        free(*out);
        *out = NULL;
    }
    return err;
}

static void directory_snapshot_free(
    argus_security_directory_snapshot_t *directory)
{
    if (directory == NULL) return;
    argus_password_zeroize(directory, sizeof(*directory));
    free(directory);
}

static bool unique_human(
    const argus_security_directory_payload_t *payload,
    const char *identifier,
    const char *login)
{
    for (size_t i = 0U; i < payload->human_count; ++i) {
        if (strcmp(payload->humans[i].identifier, identifier) == 0 ||
            strcmp(payload->humans[i].login, login) == 0) {
            return false;
        }
    }
    return true;
}

static argus_permission_set_t requested_permissions(
    argus_security_level_t level,
    argus_permission_set_t direct)
{
    return argus_authorization_level_permissions(level) | direct;
}

static argus_permission_set_t human_permissions(
    const argus_security_directory_payload_t *payload,
    const argus_security_human_record_t *human)
{
    argus_permission_set_t result = human->direct_permissions;
    for (size_t bit = 0U; bit < ARGUS_SECURITY_MAX_ROLES; ++bit) {
        if ((human->role_mask & (UINT16_C(1) << bit)) == 0U) continue;
        if (bit < ARGUS_SECURITY_BUILTIN_ROLE_COUNT) {
            result |= argus_authorization_level_permissions(
                (argus_security_level_t)bit);
        } else {
            size_t custom = bit - ARGUS_SECURITY_BUILTIN_ROLE_COUNT;
            if (custom >= payload->custom_role_count) {
                return UINT64_MAX;
            }
            result |= payload->custom_roles[custom].permissions;
        }
    }
    return result;
}

static bool is_enabled_human_admin(
    const argus_security_directory_payload_t *payload,
    const argus_security_human_record_t *human)
{
    return human->enabled != 0U && human->revoked == 0U &&
           (human_permissions(payload, human) &
            ARGUS_PERMISSION_MANAGE_USERS) != 0U;
}

bool argus_security_admin_removal_would_lock_out(
    const argus_security_directory_payload_t *payload,
    size_t target_index)
{
    if (payload == NULL) return true;
    if (target_index >= payload->human_count ||
        !is_enabled_human_admin(
            payload, &payload->humans[target_index])) {
        return false;
    }
    size_t enabled_admins = 0U;
    for (size_t i = 0U; i < payload->human_count; ++i) {
        enabled_admins +=
            is_enabled_human_admin(payload, &payload->humans[i]) ? 1U : 0U;
    }
    return enabled_admins == 1U;
}

static esp_err_t target_authorized(
    const argus_principal_t *actor,
    argus_security_level_t level,
    const char *scope,
    bool protected_identity,
    argus_permission_set_t requested)
{
    argus_authorization_result_t result =
        argus_authorization_manage_target(
            actor, level, scope, protected_identity);
    if (result != ARGUS_AUTHZ_ALLOW) return ESP_ERR_NOT_ALLOWED;
    argus_permission_set_t special =
        requested & ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS;
    requested &= ~ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS;
    if (requested != 0U &&
        argus_authorization_delegate(
            actor, level, scope, requested) != ARGUS_AUTHZ_ALLOW) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (special != 0U &&
        (actor->level != ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL ||
         level != ARGUS_SECURITY_LEVEL_CLIENT_ADMIN)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

esp_err_t argus_security_admin_create_human(
    const argus_principal_t *actor,
    const char *identifier,
    const char * canonical_login,
    const char *display_name,
    const char *scope,
    argus_security_level_t level,
    argus_permission_set_t direct_permissions,
    const uint8_t *password,
    size_t password_len)
{
    if (!argus_authorization_principal_valid(actor) ||
        identifier == NULL || canonical_login == NULL ||
        display_name == NULL || scope == NULL ||
        level > ARGUS_SECURITY_LEVEL_VIEWER ||
        !argus_auth_new_password_valid(password, password_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    char normalized[ARGUS_LOGIN_NAME_MAX + 1U];
    if (!argus_auth_canonicalize_login(canonical_login, normalized)) {
        return ESP_ERR_INVALID_ARG;
    }
    argus_permission_set_t permissions =
        requested_permissions(level, direct_permissions);
    esp_err_t auth_err = target_authorized(
        actor, level, scope, false, permissions);
    if (auth_err != ESP_OK) return auth_err;
    argus_security_directory_snapshot_t *directory = NULL;
    esp_err_t err = directory_snapshot_alloc(&directory);
    if (err != ESP_OK) return err;
    if (directory->payload.human_count >= ARGUS_SECURITY_MAX_HUMANS ||
        !unique_human(
            &directory->payload, identifier, normalized)) {
        directory_snapshot_free(directory);
        return ESP_ERR_INVALID_STATE;
    }
    argus_password_verifier_t verifier;
    err = argus_auth_service_create_verifier(
        password, password_len, &verifier);
    if (err != ESP_OK) {
        directory_snapshot_free(directory);
        return err;
    }
    argus_security_human_record_t *human =
        &directory->payload.humans[directory->payload.human_count];
    *human = (argus_security_human_record_t) {
        .record_version = ARGUS_SECURITY_RECORD_VERSION,
        .level = (uint8_t)level,
        .enabled = 1U,
        .protected_identity = 0U,
        .role_mask = (uint16_t)(UINT16_C(1) << level),
        .direct_permissions = direct_permissions,
        .credential_version = 1U,
        .record_security_epoch = directory->generation + 1U,
        .verifier = verifier,
    };
    strlcpy(human->identifier, identifier, sizeof(human->identifier));
    strlcpy(human->login, normalized, sizeof(human->login));
    strlcpy(human->display_name, display_name, sizeof(human->display_name));
    strlcpy(human->scope, scope, sizeof(human->scope));
    directory->payload.human_count++;
    err = argus_security_directory_commit(
        &directory->payload, directory->generation);
    argus_password_zeroize(&verifier, sizeof(verifier));
    directory_snapshot_free(directory);
    return err;
}

static esp_err_t snapshot_target(
    const argus_principal_t *actor,
    const char *identifier,
    argus_security_directory_snapshot_t **directory,
    size_t *index)
{
    if (!argus_authorization_principal_valid(actor) ||
        identifier == NULL || directory == NULL || index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = directory_snapshot_alloc(directory);
    if (err != ESP_OK) return err;
    for (size_t i = 0U; i < (*directory)->payload.human_count; ++i) {
        argus_security_human_record_t *human =
            &(*directory)->payload.humans[i];
        if (strcmp(human->identifier, identifier) != 0) continue;
        err = target_authorized(
            actor, (argus_security_level_t)human->level,
            human->scope, human->protected_identity != 0U,
            human_permissions(&(*directory)->payload, human));
        if (err != ESP_OK) {
            directory_snapshot_free(*directory);
            *directory = NULL;
            return err;
        }
        *index = i;
        return ESP_OK;
    }
    directory_snapshot_free(*directory);
    *directory = NULL;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t argus_security_admin_set_enabled(
    const argus_principal_t *actor,
    const char *identifier,
    bool enabled)
{
    argus_security_directory_snapshot_t *directory = NULL;
    size_t index;
    esp_err_t err = snapshot_target(
        actor, identifier, &directory, &index);
    if (err != ESP_OK) return err;
    argus_security_human_record_t *human =
        &directory->payload.humans[index];
    if (!enabled &&
        argus_security_admin_removal_would_lock_out(
            &directory->payload, index)) {
        directory_snapshot_free(directory);
        return ESP_ERR_INVALID_STATE;
    }
    human->enabled = enabled ? 1U : 0U;
    human->revoked = enabled ? 0U : 1U;
    human->record_security_epoch++;
    if (human->record_security_epoch == 0U) {
        human->record_security_epoch = 1U;
    }
    err = argus_security_directory_commit(
        &directory->payload, directory->generation);
    if (err == ESP_OK) {
        (void)argus_session_manager_revoke_principal(identifier);
    }
    directory_snapshot_free(directory);
    return err;
}

esp_err_t argus_security_admin_replace_password(
    const argus_principal_t *actor,
    const char *identifier,
    const uint8_t *password,
    size_t password_len)
{
    if (!argus_auth_new_password_valid(password, password_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    argus_security_directory_snapshot_t *directory = NULL;
    size_t index;
    esp_err_t err = snapshot_target(
        actor, identifier, &directory, &index);
    if (err != ESP_OK) return err;
    argus_password_verifier_t verifier;
    err = argus_auth_service_create_verifier(
        password, password_len, &verifier);
    if (err == ESP_OK) {
        argus_security_human_record_t *human =
            &directory->payload.humans[index];
        human->verifier = verifier;
        human->credential_version++;
        if (human->credential_version == 0U) {
            human->credential_version = 1U;
        }
        human->record_security_epoch++;
        if (human->record_security_epoch == 0U) {
            human->record_security_epoch = 1U;
        }
        err = argus_security_directory_commit(
            &directory->payload, directory->generation);
    }
    if (err == ESP_OK) {
        (void)argus_session_manager_revoke_principal(identifier);
    }
    argus_password_zeroize(&verifier, sizeof(verifier));
    directory_snapshot_free(directory);
    return err;
}

esp_err_t argus_security_admin_replace_own_password(
    const argus_principal_t *actor,
    const uint8_t *password,
    size_t password_len)
{
    if (!argus_authorization_principal_valid(actor) ||
        actor->type != ARGUS_PRINCIPAL_HUMAN ||
        !argus_auth_new_password_valid(password, password_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    argus_security_directory_snapshot_t *directory = NULL;
    esp_err_t err = directory_snapshot_alloc(&directory);
    if (err != ESP_OK) return err;
    size_t index = ARGUS_SECURITY_MAX_HUMANS;
    for (size_t i = 0U; i < directory->payload.human_count; ++i) {
        if (strcmp(directory->payload.humans[i].identifier,
                   actor->identifier) == 0) {
            index = i;
            break;
        }
    }
    if (index == ARGUS_SECURITY_MAX_HUMANS) {
        directory_snapshot_free(directory);
        return ESP_ERR_NOT_FOUND;
    }
    argus_password_verifier_t verifier;
    err = argus_auth_service_create_verifier(
        password, password_len, &verifier);
    if (err == ESP_OK) {
        argus_security_human_record_t *human =
            &directory->payload.humans[index];
        human->verifier = verifier;
        human->credential_version++;
        if (human->credential_version == 0U) human->credential_version = 1U;
        human->record_security_epoch++;
        if (human->record_security_epoch == 0U) {
            human->record_security_epoch = 1U;
        }
        err = argus_security_directory_commit(
            &directory->payload, directory->generation);
    }
    if (err == ESP_OK) {
        (void)argus_session_manager_revoke_principal(actor->identifier);
    }
    argus_password_zeroize(&verifier, sizeof(verifier));
    directory_snapshot_free(directory);
    return err;
}

esp_err_t argus_security_admin_update_display_name(
    const argus_principal_t *actor,
    const char *identifier,
    const char *display_name)
{
    if (display_name == NULL ||
        strnlen(display_name, ARGUS_SECURITY_DISPLAY_MAX + 1U) == 0U ||
        strnlen(display_name, ARGUS_SECURITY_DISPLAY_MAX + 1U) >
            ARGUS_SECURITY_DISPLAY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    argus_security_directory_snapshot_t *directory = NULL;
    size_t index;
    esp_err_t err = snapshot_target(
        actor, identifier, &directory, &index);
    if (err != ESP_OK) return err;
    strlcpy(directory->payload.humans[index].display_name, display_name,
            sizeof(directory->payload.humans[index].display_name));
    err = argus_security_directory_commit(
        &directory->payload, directory->generation);
    directory_snapshot_free(directory);
    return err;
}

esp_err_t argus_security_admin_assign_custom_roles(
    const argus_principal_t *actor,
    const char *identifier,
    uint16_t custom_role_mask)
{
    argus_security_directory_snapshot_t *directory = NULL;
    size_t index;
    esp_err_t err = snapshot_target(
        actor, identifier, &directory, &index);
    if (err != ESP_OK) return err;
    uint16_t valid_mask = directory->payload.custom_role_count == 0U
        ? 0U
        : (uint16_t)((UINT16_C(1) <<
             directory->payload.custom_role_count) - 1U);
    if ((custom_role_mask & ~valid_mask) != 0U) {
        directory_snapshot_free(directory);
        return ESP_ERR_INVALID_ARG;
    }
    argus_permission_set_t permissions = 0U;
    for (size_t i = 0U; i < directory->payload.custom_role_count; ++i) {
        if ((custom_role_mask & (UINT16_C(1) << i)) != 0U) {
            permissions |= directory->payload.custom_roles[i].permissions;
        }
    }
    argus_security_human_record_t *human =
        &directory->payload.humans[index];
    if (permissions != 0U &&
        argus_authorization_delegate(
            actor, (argus_security_level_t)human->level,
            human->scope, permissions) != ARGUS_AUTHZ_ALLOW) {
        directory_snapshot_free(directory);
        return ESP_ERR_NOT_ALLOWED;
    }
    uint16_t builtin =
        (uint16_t)(UINT16_C(1) << human->level);
    human->role_mask = builtin |
        (uint16_t)(custom_role_mask << ARGUS_SECURITY_BUILTIN_ROLE_COUNT);
    human->record_security_epoch++;
    if (human->record_security_epoch == 0U) {
        human->record_security_epoch = 1U;
    }
    err = argus_security_directory_commit(
        &directory->payload, directory->generation);
    if (err == ESP_OK) {
        (void)argus_session_manager_revoke_principal(identifier);
    }
    directory_snapshot_free(directory);
    return err;
}

esp_err_t argus_security_admin_delete_human(
    const argus_principal_t *actor,
    const char *identifier)
{
    argus_security_directory_snapshot_t *directory = NULL;
    size_t index;
    esp_err_t err = snapshot_target(
        actor, identifier, &directory, &index);
    if (err != ESP_OK) return err;
    if (argus_security_admin_removal_would_lock_out(
            &directory->payload, index)) {
        directory_snapshot_free(directory);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = index + 1U;
         i < directory->payload.human_count; ++i) {
        directory->payload.humans[i - 1U] =
            directory->payload.humans[i];
    }
    directory->payload.human_count--;
    memset(&directory->payload.humans[directory->payload.human_count],
           0, sizeof(directory->payload.humans[0]));
    err = argus_security_directory_commit(
        &directory->payload, directory->generation);
    if (err == ESP_OK) {
        (void)argus_session_manager_revoke_principal(identifier);
    }
    directory_snapshot_free(directory);
    return err;
}

esp_err_t argus_security_admin_create_role(
    const argus_principal_t *actor,
    const char *identifier,
    argus_security_level_t level,
    argus_permission_set_t permissions,
    argus_permission_set_t delegable_permissions)
{
    if (!argus_authorization_principal_valid(actor) ||
        argus_authorization_require(
            actor, ARGUS_PERMISSION_MANAGE_ROLES) != ARGUS_AUTHZ_ALLOW ||
        identifier == NULL ||
        level < ARGUS_SECURITY_LEVEL_CLIENT_ADMIN ||
        level > ARGUS_SECURITY_LEVEL_VIEWER ||
        (delegable_permissions & ~permissions) != 0U ||
        argus_authorization_delegate(
            actor, level, actor->scope, permissions) != ARGUS_AUTHZ_ALLOW ||
        (delegable_permissions & ~actor->delegable_permissions) != 0U) {
        return ESP_ERR_NOT_ALLOWED;
    }
    argus_security_directory_snapshot_t *directory = NULL;
    esp_err_t err = directory_snapshot_alloc(&directory);
    if (err != ESP_OK) return err;
    if (directory->payload.custom_role_count >=
        ARGUS_SECURITY_CUSTOM_ROLE_CAPACITY) {
        directory_snapshot_free(directory);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0U; i < directory->payload.custom_role_count; ++i) {
        if (strcmp(directory->payload.custom_roles[i].identifier,
                   identifier) == 0) {
            directory_snapshot_free(directory);
            return ESP_ERR_INVALID_STATE;
        }
    }
    argus_security_role_record_t *role =
        &directory->payload.custom_roles[
            directory->payload.custom_role_count];
    *role = (argus_security_role_record_t) {
        .record_version = ARGUS_SECURITY_RECORD_VERSION,
        .level = (uint8_t)level,
        .builtin = 0U,
        .protected_role = 0U,
        .permissions = permissions,
        .delegable_permissions = delegable_permissions,
    };
    strlcpy(role->identifier, identifier, sizeof(role->identifier));
    directory->payload.custom_role_count++;
    err = argus_security_directory_commit(
        &directory->payload, directory->generation);
    directory_snapshot_free(directory);
    return err;
}

esp_err_t argus_security_admin_delete_role(
    const argus_principal_t *actor,
    const char *identifier)
{
    if (!argus_authorization_principal_valid(actor) ||
        argus_authorization_require(
            actor, ARGUS_PERMISSION_MANAGE_ROLES) != ARGUS_AUTHZ_ALLOW ||
        identifier == NULL) {
        return ESP_ERR_NOT_ALLOWED;
    }
    argus_security_directory_snapshot_t *directory = NULL;
    esp_err_t err = directory_snapshot_alloc(&directory);
    if (err != ESP_OK) return err;
    size_t index = ARGUS_SECURITY_CUSTOM_ROLE_CAPACITY;
    for (size_t i = 0U; i < directory->payload.custom_role_count; ++i) {
        if (strcmp(directory->payload.custom_roles[i].identifier,
                   identifier) == 0) {
            index = i;
            break;
        }
    }
    if (index == ARGUS_SECURITY_CUSTOM_ROLE_CAPACITY) {
        directory_snapshot_free(directory);
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t role_bit = (uint16_t)(UINT16_C(1) <<
        (index + ARGUS_SECURITY_BUILTIN_ROLE_COUNT));
    for (size_t i = 0U; i < directory->payload.human_count; ++i) {
        if ((directory->payload.humans[i].role_mask & role_bit) != 0U) {
            directory_snapshot_free(directory);
            return ESP_ERR_INVALID_STATE;
        }
    }
    for (size_t i = index + 1U;
         i < directory->payload.custom_role_count; ++i) {
        directory->payload.custom_roles[i - 1U] =
            directory->payload.custom_roles[i];
    }
    directory->payload.custom_role_count--;
    uint16_t lower = (uint16_t)(role_bit - 1U);
    for (size_t i = 0U; i < directory->payload.human_count; ++i) {
        uint16_t mask = directory->payload.humans[i].role_mask;
        uint16_t below = mask & lower;
        uint16_t above = mask & (uint16_t)~(lower | role_bit);
        directory->payload.humans[i].role_mask =
            below | (uint16_t)(above >> 1U);
    }
    memset(&directory->payload.custom_roles[
               directory->payload.custom_role_count],
           0, sizeof(directory->payload.custom_roles[0]));
    err = argus_security_directory_commit(
        &directory->payload, directory->generation);
    directory_snapshot_free(directory);
    return err;
}
