#ifndef ARGUS_MACHINE_SERVICE_H
#define ARGUS_MACHINE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_authorization.h"
#include "argus_security_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_MACHINE_ID_LENGTH 32U
#define ARGUS_MACHINE_SECRET_BYTES 32U
#define ARGUS_MACHINE_SECRET_LENGTH 43U
#define ARGUS_MACHINE_AUTH_BUCKETS 8U
#define ARGUS_MACHINE_AUTH_FAILURE_LIMIT 5U
#define ARGUS_MACHINE_AUTH_WINDOW_US UINT64_C(60000000)
#define ARGUS_MACHINE_AUTH_COOLDOWN_US UINT64_C(30000000)
#define ARGUS_MACHINE_AUTH_KDF_ADMISSION_MAX 2U

typedef struct {
    char display_name[ARGUS_SECURITY_DISPLAY_MAX + 1U];
    argus_machine_client_type_t client_type;
    uint8_t allowed_transports;
    uint8_t allowed_interfaces;
    char scope[ARGUS_SECURITY_SCOPE_MAX + 1U];
    char topic_scope[ARGUS_SECURITY_TOPIC_SCOPE_MAX + 1U];
    char api_scope[ARGUS_SECURITY_API_SCOPE_MAX + 1U];
    argus_permission_set_t permissions;
} argus_machine_enrollment_request_t;

typedef struct {
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
    char secret[ARGUS_MACHINE_SECRET_LENGTH + 1U];
    uint32_t credential_version;
    uint32_t principal_revision;
} argus_machine_credential_once_t;

typedef struct {
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
    char scope[ARGUS_SECURITY_SCOPE_MAX + 1U];
    char topic_scope[ARGUS_SECURITY_TOPIC_SCOPE_MAX + 1U];
    char api_scope[ARGUS_SECURITY_API_SCOPE_MAX + 1U];
    argus_permission_set_t permissions;
    uint8_t allowed_transports;
    uint8_t allowed_interfaces;
    uint8_t client_type;
    uint32_t credential_version;
    uint32_t principal_revision;
    uint32_t record_security_epoch;
    uint32_t directory_generation;
} argus_machine_principal_t;

typedef enum {
    ARGUS_MACHINE_AUTH_SUCCESS = 0,
    ARGUS_MACHINE_AUTH_INVALID_CREDENTIALS,
    ARGUS_MACHINE_AUTH_THROTTLED,
    ARGUS_MACHINE_AUTH_BUSY,
    ARGUS_MACHINE_AUTH_DIRECTORY_UNAVAILABLE,
    ARGUS_MACHINE_AUTH_TRANSPORT_REJECTED,
    ARGUS_MACHINE_AUTH_INTERFACE_REJECTED,
    ARGUS_MACHINE_AUTH_CLIENT_ID_REJECTED,
    ARGUS_MACHINE_AUTH_INVALID_REQUEST,
} argus_machine_auth_result_t;

typedef struct {
    argus_machine_auth_result_t result;
    argus_machine_principal_t principal;
    bool kdf_invoked;
    uint32_t retry_after_s;
} argus_machine_auth_outcome_t;

esp_err_t argus_machine_service_init(void);

esp_err_t argus_machine_service_enroll(
    const argus_principal_t *actor,
    const argus_machine_enrollment_request_t *request,
    argus_machine_credential_once_t *out);
esp_err_t argus_machine_service_rotate(
    const argus_principal_t *actor, const char *identifier,
    argus_machine_credential_once_t *out);
esp_err_t argus_machine_service_set_enabled(
    const argus_principal_t *actor, const char *identifier, bool enabled);
esp_err_t argus_machine_service_revoke(
    const argus_principal_t *actor, const char *identifier);
esp_err_t argus_machine_service_delete(
    const argus_principal_t *actor, const char *identifier);
esp_err_t argus_machine_service_quarantine_undisclosed(
    const char *identifier);

argus_machine_auth_outcome_t argus_machine_service_authenticate(
    uint32_t peer_key, const char *client_id,
    const uint8_t *username, size_t username_len,
    const uint8_t *password, size_t password_len,
    uint8_t receiving_interface);
esp_err_t argus_machine_service_revalidate(
    const argus_machine_principal_t *principal,
    uint8_t receiving_interface);

bool argus_machine_service_scope_contains(
    const char *actor_scope, const char *target_scope);
bool argus_machine_service_topic_scope_contains(
    const char *semantic_scope, const char *topic);
bool argus_machine_service_enrollment_allowed(
    const argus_principal_t *actor,
    const argus_machine_enrollment_request_t *request);
void argus_machine_service_zero_credential(
    argus_machine_credential_once_t *credential);
size_t argus_machine_service_kdf_admitted(void);

#ifdef __cplusplus
}
#endif

#endif
