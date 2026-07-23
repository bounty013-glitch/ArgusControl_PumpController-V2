#ifndef ARGUS_SECURITY_MIGRATION_H
#define ARGUS_SECURITY_MIGRATION_H

#include <stdbool.h>
#include <stddef.h>

#include "argus_password_verifier.h"
#include "argus_security_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool legacy_marker_present;
    bool legacy_plaintext_present;
    bool console_verifier_present;
    bool legacy_plaintext_deleted;
    bool build_default_deferred;
} argus_security_migration_status_t;

typedef struct {
    esp_err_t (*inspect_legacy)(void *ctx, uint8_t *password,
                                size_t password_size,
                                bool *marker_present,
                                bool *plaintext_present);
    esp_err_t (*get_verifier)(void *ctx,
                              argus_password_verifier_t *out);
    esp_err_t (*create_verifier)(void *ctx, const uint8_t *password,
                                 size_t password_len,
                                 argus_password_verifier_t *out);
    esp_err_t (*set_verifier)(void *ctx,
                              const argus_password_verifier_t *record);
    esp_err_t (*verify)(void *ctx, const uint8_t *password,
                        size_t password_len,
                        const argus_password_verifier_t *record,
                        bool *out_match);
    esp_err_t (*delete_legacy)(void *ctx);
    esp_err_t (*set_state)(void *ctx,
                           argus_security_migration_state_t state);
    void *ctx;
} argus_security_migration_ops_t;

esp_err_t argus_security_migration_execute(
    const argus_security_migration_ops_t *ops,
    argus_security_migration_status_t *out_status);

esp_err_t argus_security_migration_run(
    argus_security_migration_status_t *out_status);

#ifdef __cplusplus
}
#endif

#endif
