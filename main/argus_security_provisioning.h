#ifndef ARGUS_SECURITY_PROVISIONING_H
#define ARGUS_SECURITY_PROVISIONING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_password_verifier.h"
#include "argus_security_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARGUS_PROVISIONING_DEVELOPMENT = 0,
    ARGUS_PROVISIONING_PRODUCTION,
} argus_provisioning_environment_t;

typedef struct {
    argus_provisioning_environment_t environment;
    const uint8_t *factory_ap;
    size_t factory_ap_len;
    const uint8_t *active_ap;
    size_t active_ap_len;
    const uint8_t *console_password;
    size_t console_password_len;
    uint32_t verifier_iterations;
    bool explicit_initialization;
} argus_security_provisioning_request_t;

typedef struct {
    bool accepted;
    bool verifier_created;
    bool record_written;
    bool readback_verified;
    uint16_t schema_version;
    uint32_t security_epoch;
    esp_err_t error;
} argus_security_provisioning_result_t;

typedef struct {
    esp_err_t (*derive)(void *ctx, const uint8_t *password,
                        size_t password_len, uint32_t iterations,
                        argus_password_verifier_t *out);
    esp_err_t (*write_initial)(
        void *ctx, const uint8_t *factory_ap, size_t factory_ap_len,
        const uint8_t *active_ap, size_t active_ap_len,
        const argus_password_verifier_t *verifier);
    esp_err_t (*read_verifier)(void *ctx,
                               argus_password_verifier_t *out);
    esp_err_t (*verify)(void *ctx, const uint8_t *password,
                        size_t password_len,
                        const argus_password_verifier_t *record,
                        bool *out_match);
    esp_err_t (*read_status)(void *ctx,
                             argus_security_store_status_t *out);
    void *ctx;
} argus_security_provisioning_ops_t;

bool argus_security_provisioning_ap_valid(const uint8_t *value,
                                          size_t length);
argus_security_provisioning_result_t argus_security_provisioning_execute(
    const argus_security_provisioning_request_t *request,
    const argus_security_provisioning_ops_t *ops);
void argus_security_provisioning_get_production_ops(
    argus_security_provisioning_ops_t *out_ops);

#ifdef __cplusplus
}
#endif

#endif
