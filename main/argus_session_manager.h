#ifndef ARGUS_SESSION_MANAGER_H
#define ARGUS_SESSION_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_authorization.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_SESSION_CAPACITY 8U
#define ARGUS_SESSION_PER_PRINCIPAL 2U
#define ARGUS_SESSION_SECRET_BYTES 32U
#define ARGUS_SESSION_TOKEN_HEX_LEN (ARGUS_SESSION_SECRET_BYTES * 2U)
#define ARGUS_SESSION_IDLE_US UINT64_C(900000000)
#define ARGUS_SESSION_ABSOLUTE_US UINT64_C(28800000000)
#define ARGUS_SESSION_REAUTH_US UINT64_C(300000000)

typedef struct {
    bool occupied;
    uint8_t token_digest[32];
    uint8_t csrf_digest[32];
    char csrf_secret[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
    argus_principal_t principal;
    uint64_t created_us;
    uint64_t last_activity_us;
    uint64_t reauthenticated_us;
} argus_session_record_t;

typedef struct {
    argus_session_record_t records[ARGUS_SESSION_CAPACITY];
} argus_session_table_t;

typedef struct {
    char token[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
    char csrf[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
    argus_principal_t principal;
    uint64_t expires_us;
} argus_session_credentials_t;

typedef esp_err_t (*argus_session_random_fn)(
    void *ctx, uint8_t *out, size_t length);

void argus_session_table_init(argus_session_table_t *table);
esp_err_t argus_session_create(
    argus_session_table_t *table,
    const argus_principal_t *principal,
    uint64_t now_us,
    argus_session_random_fn random_fn,
    void *random_ctx,
    argus_session_credentials_t *out);
esp_err_t argus_session_authenticate(
    argus_session_table_t *table,
    const char *token,
    uint64_t now_us,
    argus_principal_t *out_principal,
    size_t *out_index);
bool argus_session_csrf_valid(
    const argus_session_table_t *table,
    size_t index,
    const char *csrf);
bool argus_session_get_csrf(
    const argus_session_table_t *table,
    size_t index,
    char out[ARGUS_SESSION_TOKEN_HEX_LEN + 1U]);
bool argus_session_recently_reauthenticated(
    const argus_session_table_t *table,
    size_t index,
    uint64_t now_us);
void argus_session_mark_reauthenticated(
    argus_session_table_t *table,
    size_t index,
    uint64_t now_us);
size_t argus_session_revoke_principal(
    argus_session_table_t *table,
    const char *principal_id);
bool argus_session_revoke_token(
    argus_session_table_t *table,
    const char *token);
size_t argus_session_revoke_epoch(
    argus_session_table_t *table,
    uint32_t current_security_epoch);
size_t argus_session_count(const argus_session_table_t *table);
size_t argus_session_revoke_all(argus_session_table_t *table);

esp_err_t argus_session_manager_init(void);
esp_err_t argus_session_manager_create(
    const argus_principal_t *principal,
    argus_session_credentials_t *out);
esp_err_t argus_session_manager_authenticate(
    const char *token,
    argus_principal_t *out_principal,
    size_t *out_index);
bool argus_session_manager_csrf_valid(size_t index, const char *csrf);
bool argus_session_manager_get_csrf(
    size_t index,
    char out[ARGUS_SESSION_TOKEN_HEX_LEN + 1U]);
bool argus_session_manager_recently_reauthenticated(size_t index);
void argus_session_manager_mark_reauthenticated(size_t index);
bool argus_session_manager_revoke_token(const char *token);
size_t argus_session_manager_revoke_principal(const char *principal_id);
size_t argus_session_manager_revoke_epoch(uint32_t current_security_epoch);
size_t argus_session_manager_count(void);
size_t argus_session_manager_revoke_all(void);

#ifdef __cplusplus
}
#endif

#endif
