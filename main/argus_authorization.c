#include "argus_authorization.h"

#include <string.h>

static const argus_permission_set_t OPERATIONAL =
    ARGUS_PERMISSION_VIEW_STATUS | ARGUS_PERMISSION_REQUEST_AUTHORITY |
    ARGUS_PERMISSION_MOTION | ARGUS_PERMISSION_SOFTWARE_ESTOP |
    ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP | ARGUS_PERMISSION_ACK_ALARMS;

argus_permission_set_t argus_authorization_level_permissions(
    argus_security_level_t level)
{
    switch (level) {
        case ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL:
            return ARGUS_PERMISSION_DEFINED_MASK;
        case ARGUS_SECURITY_LEVEL_CLIENT_ADMIN:
            return OPERATIONAL | ARGUS_PERMISSION_MANAGE_USERS |
                   ARGUS_PERMISSION_MANAGE_ROLES |
                   ARGUS_PERMISSION_ENROLL_MACHINES |
                   ARGUS_PERMISSION_REVOKE_MACHINES |
                   ARGUS_PERMISSION_VIEW_AUDIT;
        case ARGUS_SECURITY_LEVEL_SUPERVISOR:
        case ARGUS_SECURITY_LEVEL_OPERATOR:
            return OPERATIONAL;
        case ARGUS_SECURITY_LEVEL_VIEWER:
            return ARGUS_PERMISSION_VIEW_STATUS;
        default:
            return 0U;
    }
}

argus_permission_set_t argus_authorization_level_delegable(
    argus_security_level_t level)
{
    argus_permission_set_t result =
        argus_authorization_level_permissions(level);
    result &= ~(ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS |
                ARGUS_PERMISSION_MODIFY_IDENTITY |
                ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG |
                ARGUS_PERMISSION_COMMISSION |
                ARGUS_PERMISSION_CALIBRATE |
                ARGUS_PERMISSION_MANAGE_FIRMWARE |
                ARGUS_PERMISSION_FULL_SECURITY_RESET);
    return result;
}

bool argus_authorization_principal_valid(const argus_principal_t *principal)
{
    if (principal == NULL ||
        (principal->type != ARGUS_PRINCIPAL_CONSOLE &&
         principal->type != ARGUS_PRINCIPAL_HUMAN) ||
        principal->level > ARGUS_SECURITY_LEVEL_VIEWER ||
        principal->identifier[0] == '\0' || principal->scope[0] == '\0' ||
        principal->credential_version == 0U ||
        principal->security_epoch == 0U ||
        principal->principal_revision == 0U ||
        (principal->permissions & ~ARGUS_PERMISSION_DEFINED_MASK) != 0U ||
        (principal->delegable_permissions & ~principal->permissions) != 0U ||
        (principal->delegable_permissions &
         ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) != 0U) {
        return false;
    }
    return strnlen(principal->identifier,
                   sizeof(principal->identifier)) <
               sizeof(principal->identifier) &&
           strnlen(principal->scope, sizeof(principal->scope)) <
               sizeof(principal->scope);
}

argus_authorization_result_t argus_authorization_require(
    const argus_principal_t *principal,
    argus_permission_set_t required)
{
    if (!argus_authorization_principal_valid(principal) || required == 0U ||
        (required & ~ARGUS_PERMISSION_DEFINED_MASK) != 0U) {
        return ARGUS_AUTHZ_DENY_INVALID;
    }
    return (principal->permissions & required) == required
               ? ARGUS_AUTHZ_ALLOW
               : ARGUS_AUTHZ_DENY_CAPABILITY;
}

static bool scope_contains(const char *actor, const char *target)
{
    return actor != NULL && target != NULL &&
           (strcmp(actor, "*") == 0 || strcmp(actor, target) == 0);
}

argus_authorization_result_t argus_authorization_manage_target(
    const argus_principal_t *actor,
    argus_security_level_t target_level,
    const char *target_scope,
    bool target_protected)
{
    argus_authorization_result_t capability =
        argus_authorization_require(actor, ARGUS_PERMISSION_MANAGE_USERS);
    if (capability != ARGUS_AUTHZ_ALLOW) return capability;
    if (target_scope == NULL || target_level > ARGUS_SECURITY_LEVEL_VIEWER) {
        return ARGUS_AUTHZ_DENY_INVALID;
    }
    if (target_protected ||
        target_level == ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL) {
        return actor->level == ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL
                   ? ARGUS_AUTHZ_ALLOW
                   : ARGUS_AUTHZ_DENY_PROTECTED;
    }
    if (!scope_contains(actor->scope, target_scope)) {
        return ARGUS_AUTHZ_DENY_SCOPE;
    }
    if (target_level < actor->level) return ARGUS_AUTHZ_DENY_LEVEL;
    if (target_level == ARGUS_SECURITY_LEVEL_CLIENT_ADMIN &&
        (actor->permissions & ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) == 0U) {
        return ARGUS_AUTHZ_DENY_CAPABILITY;
    }
    if (actor->level > ARGUS_SECURITY_LEVEL_CLIENT_ADMIN &&
        target_level < ARGUS_SECURITY_LEVEL_OPERATOR) {
        return ARGUS_AUTHZ_DENY_LEVEL;
    }
    return ARGUS_AUTHZ_ALLOW;
}

argus_authorization_result_t argus_authorization_delegate(
    const argus_principal_t *actor,
    argus_security_level_t target_level,
    const char *target_scope,
    argus_permission_set_t requested)
{
    argus_authorization_result_t target = argus_authorization_manage_target(
        actor, target_level, target_scope, false);
    if (target != ARGUS_AUTHZ_ALLOW) return target;
    if (requested == 0U ||
        (requested & ~ARGUS_PERMISSION_DEFINED_MASK) != 0U) {
        return ARGUS_AUTHZ_DENY_INVALID;
    }
    if ((requested & ~actor->permissions) != 0U ||
        (requested & ~actor->delegable_permissions) != 0U ||
        (requested & ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) != 0U) {
        return ARGUS_AUTHZ_DENY_DELEGATION;
    }
    return ARGUS_AUTHZ_ALLOW;
}
