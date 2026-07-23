#ifndef ARGUS_AUTHORIZATION_H
#define ARGUS_AUTHORIZATION_H

#include <stdbool.h>
#include <stdint.h>

#include "argus_security_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARGUS_PRINCIPAL_NONE = 0,
    ARGUS_PRINCIPAL_CONSOLE,
    ARGUS_PRINCIPAL_HUMAN,
} argus_principal_type_t;

typedef struct {
    argus_principal_type_t type;
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
    char scope[ARGUS_SECURITY_SCOPE_MAX + 1U];
    argus_security_level_t level;
    argus_permission_set_t permissions;
    argus_permission_set_t delegable_permissions;
    uint32_t credential_version;
    uint32_t security_epoch;
    uint32_t principal_revision;
    bool protected_identity;
} argus_principal_t;

typedef enum {
    ARGUS_AUTHZ_ALLOW = 0,
    ARGUS_AUTHZ_DENY_INVALID,
    ARGUS_AUTHZ_DENY_CAPABILITY,
    ARGUS_AUTHZ_DENY_SCOPE,
    ARGUS_AUTHZ_DENY_LEVEL,
    ARGUS_AUTHZ_DENY_DELEGATION,
    ARGUS_AUTHZ_DENY_PROTECTED,
} argus_authorization_result_t;

bool argus_authorization_principal_valid(const argus_principal_t *principal);
argus_authorization_result_t argus_authorization_require(
    const argus_principal_t *principal,
    argus_permission_set_t required);
argus_authorization_result_t argus_authorization_manage_target(
    const argus_principal_t *actor,
    argus_security_level_t target_level,
    const char *target_scope,
    bool target_protected);
argus_authorization_result_t argus_authorization_delegate(
    const argus_principal_t *actor,
    argus_security_level_t target_level,
    const char *target_scope,
    argus_permission_set_t requested);
argus_permission_set_t argus_authorization_level_permissions(
    argus_security_level_t level);
argus_permission_set_t argus_authorization_level_delegable(
    argus_security_level_t level);

#ifdef __cplusplus
}
#endif

#endif
