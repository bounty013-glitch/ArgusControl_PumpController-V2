#include "argus_security_migration.h"

#include <string.h>

#include "argus_password_verifier.h"
#include "argus_security_store.h"
#include "esp_log.h"
#include "nvs.h"

#define LEGACY_PORTAL_NAMESPACE "argus_portal"
#define LEGACY_PORTAL_PASSWORD_KEY "pw"
#define LEGACY_PORTAL_MARKER_KEY "pw_set"

static const char *TAG = "argus_sec_migrate";

static esp_err_t inspect_legacy(uint8_t *password_bytes, size_t password_size,
                                bool *marker_present,
                                bool *plaintext_present)
{
    char *password = (char *)password_bytes;
    *marker_present = false;
    *plaintext_present = false;
    password[0] = '\0';
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LEGACY_PORTAL_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t marker = 0U;
    err = nvs_get_u8(handle, LEGACY_PORTAL_MARKER_KEY, &marker);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK || marker != 1U) {
        nvs_close(handle);
        return ESP_ERR_INVALID_STATE;
    }
    *marker_present = true;
    size_t length = password_size;
    err = nvs_get_str(handle, LEGACY_PORTAL_PASSWORD_KEY, password, &length);
    nvs_close(handle);
    if (err != ESP_OK || length <= 1U || length > password_size) {
        password[0] = '\0';
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    *plaintext_present = true;
    return ESP_OK;
}

static esp_err_t delete_legacy_plaintext(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LEGACY_PORTAL_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, LEGACY_PORTAL_PASSWORD_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) {
        esp_err_t marker_err =
            nvs_erase_key(handle, LEGACY_PORTAL_MARKER_KEY);
        if (marker_err != ESP_OK && marker_err != ESP_ERR_NVS_NOT_FOUND) {
            err = marker_err;
        }
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t argus_security_migration_execute(
    const argus_security_migration_ops_t *ops,
    argus_security_migration_status_t *out_status)
{
    if (ops == NULL || out_status == NULL || ops->inspect_legacy == NULL ||
        ops->get_verifier == NULL || ops->create_verifier == NULL ||
        ops->set_verifier == NULL || ops->verify == NULL ||
        ops->delete_legacy == NULL || ops->set_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    uint8_t legacy_password[ARGUS_PASSWORD_INPUT_MAX + 1U] = {0};
    esp_err_t err = ops->inspect_legacy(
        ops->ctx,
        legacy_password, sizeof(legacy_password),
        &out_status->legacy_marker_present,
        &out_status->legacy_plaintext_present);
    if (err != ESP_OK) {
        (void)ops->set_state(ops->ctx, ARGUS_SECURITY_MIGRATION_FAILED);
        argus_password_zeroize(legacy_password, sizeof(legacy_password));
        return err;
    }

    argus_password_verifier_t stored = {0};
    esp_err_t stored_err = ops->get_verifier(ops->ctx, &stored);
    out_status->console_verifier_present = stored_err == ESP_OK;

    if (!out_status->legacy_plaintext_present) {
        argus_password_zeroize(&stored, sizeof(stored));
        if (stored_err == ESP_OK) {
            return ops->set_state(ops->ctx,
                                  ARGUS_SECURITY_MIGRATION_COMPLETE);
        }
        if (stored_err != ESP_ERR_NOT_FOUND) return stored_err;
        out_status->build_default_deferred = true;
        return ops->set_state(
            ops->ctx, ARGUS_SECURITY_MIGRATION_BUILD_DEFAULT_DEFERRED);
    }

    if (stored_err == ESP_ERR_NOT_FOUND) {
        argus_password_verifier_t created = {0};
        size_t password_len = strnlen((const char *)legacy_password,
                                      sizeof(legacy_password));
        err = ops->create_verifier(ops->ctx, legacy_password,
                                   password_len, &created);
        if (err == ESP_OK) {
            err = ops->set_verifier(ops->ctx, &created);
        }
        argus_password_zeroize(&created, sizeof(created));
        if (err != ESP_OK) goto fail;
        err = ops->get_verifier(ops->ctx, &stored);
        if (err != ESP_OK) goto fail;
        out_status->console_verifier_present = true;
    } else if (stored_err != ESP_OK) {
        err = stored_err;
        goto fail;
    }

    bool match = false;
    err = ops->verify(
        ops->ctx, legacy_password,
        strnlen((const char *)legacy_password, sizeof(legacy_password)),
        &stored, &match);
    if (err != ESP_OK || !match) {
        if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    err = ops->delete_legacy(ops->ctx);
    if (err != ESP_OK) goto fail;
    out_status->legacy_plaintext_deleted = true;
    err = ops->set_state(ops->ctx, ARGUS_SECURITY_MIGRATION_COMPLETE);
    argus_password_zeroize(&stored, sizeof(stored));
    argus_password_zeroize(legacy_password, sizeof(legacy_password));
    return err;

fail:
    (void)ops->set_state(ops->ctx, ARGUS_SECURITY_MIGRATION_FAILED);
    argus_password_zeroize(&stored, sizeof(stored));
    argus_password_zeroize(legacy_password, sizeof(legacy_password));
    return err;
}

static esp_err_t prod_inspect(void *ctx, uint8_t *password,
                              size_t password_size, bool *marker_present,
                              bool *plaintext_present)
{
    (void)ctx;
    return inspect_legacy(password, password_size, marker_present,
                          plaintext_present);
}

static esp_err_t prod_get_verifier(void *ctx,
                                   argus_password_verifier_t *out)
{
    (void)ctx;
    return argus_security_store_get_console_verifier(out, NULL);
}

static esp_err_t prod_create_verifier(void *ctx, const uint8_t *password,
                                      size_t password_len,
                                      argus_password_verifier_t *out)
{
    (void)ctx;
    return argus_password_verifier_create(
        password, password_len, ARGUS_PASSWORD_ITERATIONS_DEFAULT, out);
}

static esp_err_t prod_set_verifier(
    void *ctx, const argus_password_verifier_t *record)
{
    (void)ctx;
    return argus_security_store_set_console_verifier(record, false);
}

static esp_err_t prod_verify(void *ctx, const uint8_t *password,
                             size_t password_len,
                             const argus_password_verifier_t *record,
                             bool *out_match)
{
    (void)ctx;
    return argus_password_verifier_verify(password, password_len, record,
                                           out_match);
}

static esp_err_t prod_delete_legacy(void *ctx)
{
    (void)ctx;
    return delete_legacy_plaintext();
}

static esp_err_t prod_set_state(void *ctx,
                                argus_security_migration_state_t state)
{
    (void)ctx;
    return argus_security_store_set_migration_state(state);
}

esp_err_t argus_security_migration_run(
    argus_security_migration_status_t *out_status)
{
    const argus_security_migration_ops_t ops = {
        .inspect_legacy = prod_inspect,
        .get_verifier = prod_get_verifier,
        .create_verifier = prod_create_verifier,
        .set_verifier = prod_set_verifier,
        .verify = prod_verify,
        .delete_legacy = prod_delete_legacy,
        .set_state = prod_set_state,
    };
    esp_err_t err = argus_security_migration_execute(&ops, out_status);
    if (err == ESP_OK && out_status != NULL &&
        out_status->legacy_plaintext_deleted) {
        ESP_LOGI(TAG, "Legacy console credential migrated and plaintext removed");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Legacy console credential migration failed closed: %s",
                 esp_err_to_name(err));
    }
    return err;
}
