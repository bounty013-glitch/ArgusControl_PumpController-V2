#include "argus_security_provisioning.h"

#include <string.h>

bool argus_security_provisioning_ap_valid(const uint8_t *value,
                                          size_t length)
{
    if (value == NULL || length < ARGUS_SECURITY_AP_SECRET_MIN ||
        length > ARGUS_SECURITY_AP_SECRET_MAX) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        if (value[i] < 0x20U || value[i] > 0x7eU) return false;
    }
    return true;
}

argus_security_provisioning_result_t argus_security_provisioning_execute(
    const argus_security_provisioning_request_t *request,
    const argus_security_provisioning_ops_t *ops)
{
    argus_security_provisioning_result_t result = {.error = ESP_OK};
    if (request == NULL || ops == NULL || ops->derive == NULL ||
        ops->write_initial == NULL || ops->read_verifier == NULL ||
        ops->verify == NULL || ops->read_status == NULL ||
        request->environment > ARGUS_PROVISIONING_PRODUCTION ||
        !request->explicit_initialization ||
        !argus_security_provisioning_ap_valid(
            request->factory_ap, request->factory_ap_len) ||
        !argus_security_provisioning_ap_valid(
            request->active_ap, request->active_ap_len) ||
        request->console_password == NULL ||
        request->console_password_len == 0U ||
        request->console_password_len > ARGUS_PASSWORD_INPUT_MAX ||
        request->verifier_iterations < ARGUS_PASSWORD_ITERATIONS_MIN ||
        request->verifier_iterations > ARGUS_PASSWORD_ITERATIONS_MAX) {
        result.error = ESP_ERR_INVALID_ARG;
        return result;
    }

    argus_password_verifier_t verifier = {0};
    result.error = ops->derive(
        ops->ctx, request->console_password,
        request->console_password_len, request->verifier_iterations,
        &verifier);
    if (result.error != ESP_OK) goto cleanup;
    result.verifier_created = true;
    result.error = ops->write_initial(
        ops->ctx, request->factory_ap, request->factory_ap_len,
        request->active_ap, request->active_ap_len, &verifier);
    if (result.error != ESP_OK) goto cleanup;
    result.record_written = true;

    argus_password_verifier_t readback = {0};
    result.error = ops->read_verifier(ops->ctx, &readback);
    if (result.error != ESP_OK) {
        argus_password_zeroize(&readback, sizeof(readback));
        goto cleanup;
    }
    bool match = false;
    result.error = ops->verify(
        ops->ctx, request->console_password,
        request->console_password_len, &readback, &match);
    argus_password_zeroize(&readback, sizeof(readback));
    if (result.error != ESP_OK || !match) {
        if (result.error == ESP_OK) result.error = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    argus_security_store_status_t status;
    result.error = ops->read_status(ops->ctx, &status);
    if (result.error != ESP_OK || !status.factory_ap_provisioned ||
        !status.active_ap_provisioned ||
        !status.console_verifier_provisioned) {
        if (result.error == ESP_OK) result.error = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    result.schema_version = status.schema_version;
    result.security_epoch = status.security_epoch;
    result.readback_verified = true;
    result.accepted = true;

cleanup:
    argus_password_zeroize(&verifier, sizeof(verifier));
    return result;
}

static esp_err_t prod_derive(void *ctx, const uint8_t *password,
                             size_t password_len, uint32_t iterations,
                             argus_password_verifier_t *out)
{
    (void)ctx;
    return argus_password_verifier_create(password, password_len,
                                           iterations, out);
}

static esp_err_t prod_write(void *ctx, const uint8_t *factory_ap,
                            size_t factory_ap_len,
                            const uint8_t *active_ap,
                            size_t active_ap_len,
                            const argus_password_verifier_t *verifier)
{
    (void)ctx;
    return argus_security_store_provision_initial(
        factory_ap, factory_ap_len, active_ap, active_ap_len, verifier);
}

static esp_err_t prod_read_verifier(void *ctx,
                                    argus_password_verifier_t *out)
{
    (void)ctx;
    return argus_security_store_get_console_verifier(out, NULL);
}

static esp_err_t prod_verify(void *ctx, const uint8_t *password,
                             size_t password_len,
                             const argus_password_verifier_t *record,
                             bool *out_match)
{
    (void)ctx;
    return argus_password_verifier_verify(password, password_len,
                                           record, out_match);
}

static esp_err_t prod_status(void *ctx,
                             argus_security_store_status_t *out)
{
    (void)ctx;
    return argus_security_store_get_status(out);
}

void argus_security_provisioning_get_production_ops(
    argus_security_provisioning_ops_t *out_ops)
{
    if (out_ops == NULL) return;
    *out_ops = (argus_security_provisioning_ops_t) {
        .derive = prod_derive,
        .write_initial = prod_write,
        .read_verifier = prod_read_verifier,
        .verify = prod_verify,
        .read_status = prod_status,
    };
}
