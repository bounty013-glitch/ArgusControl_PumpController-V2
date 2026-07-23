#ifndef ARGUS_AUTH_SERVICE_H
#define ARGUS_AUTH_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_authorization.h"
#include "argus_password_verifier.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_LOGIN_NAME_MAX ARGUS_SECURITY_LOGIN_MAX
#define ARGUS_LOGIN_BUCKET_CAPACITY 8U
#define ARGUS_LOGIN_FAILURE_LIMIT 5U
#define ARGUS_LOGIN_WINDOW_US UINT64_C(60000000)
#define ARGUS_LOGIN_COOLDOWN_INITIAL_US UINT64_C(30000000)
#define ARGUS_LOGIN_COOLDOWN_MAX_US UINT64_C(300000000)
#define ARGUS_LOGIN_KDF_ADMISSION_MAX 2U

typedef enum {
    ARGUS_LOGIN_SUCCESS = 0,
    ARGUS_LOGIN_INVALID_CREDENTIALS,
    ARGUS_LOGIN_THROTTLED,
    ARGUS_LOGIN_BUSY,
    ARGUS_LOGIN_STORE_UNAVAILABLE,
    ARGUS_LOGIN_INVALID_REQUEST,
} argus_login_result_t;

typedef struct {
    argus_login_result_t result;
    argus_principal_t principal;
    uint32_t retry_after_s;
    bool kdf_invoked;
} argus_login_outcome_t;

bool argus_auth_canonicalize_login(
    const char *input, char out[ARGUS_LOGIN_NAME_MAX + 1U]);
bool argus_auth_new_password_valid(
    const uint8_t *password, size_t length);

esp_err_t argus_auth_service_init(void);
argus_login_outcome_t argus_auth_service_authenticate(
    uint32_t peer_key,
    const char *username,
    const uint8_t *password,
    size_t password_len);
size_t argus_auth_service_kdf_admitted(void);
uint32_t argus_auth_service_kdf_invocations(void);
esp_err_t argus_auth_service_create_verifier(
    const uint8_t *password,
    size_t password_len,
    argus_password_verifier_t *out);

#ifdef __cplusplus
}
#endif

#endif
