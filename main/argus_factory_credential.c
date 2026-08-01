#include "argus_factory_credential.h"

#include <string.h>

#include "argus_auth_service.h"
#include "argus_password_verifier.h"
#include "argus_security_store.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

static const char *TAG = "argus_factory_cred";

// No 0/O/1/I/L - a password is read off a screen and typed on a phone, and
// an operator who mistypes an ambiguous character concludes the device is
// broken rather than that the font is.
static const char ALPHABET[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
#define ALPHABET_SIZE (sizeof(ALPHABET) - 1U)

static bool s_factory_checked;
static bool s_factory_in_use;

static const char *configured_salt(void)
{
#ifdef CONFIG_ARGUS_FACTORY_CREDENTIAL_SALT
    return CONFIG_ARGUS_FACTORY_CREDENTIAL_SALT;
#else
    return "";
#endif
}

esp_err_t argus_factory_credential_derive(char *out, size_t cap)
{
    if (out == NULL || cap <= ARGUS_FACTORY_CREDENTIAL_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        return err;
    }

    const char *salt = configured_salt();
    size_t salt_len = strlen(salt);
    if (salt_len == 0U) {
        // Say this every time it is asked, not once at boot. A build whose
        // portal password is derivable from a beacon frame must never be
        // mistaken for a provisioned one.
        ESP_LOGW(TAG, "NO FACTORY SALT CONFIGURED. This device's portal "
                      "password is derivable by anyone who can see its "
                      "service SSID. Set ARGUS_FACTORY_CREDENTIAL_SALT "
                      "before this leaves a bench.");
    }

    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc == 0 && salt_len > 0U) {
        rc = mbedtls_sha256_update(&ctx, (const uint8_t *)salt, salt_len);
    }
    if (rc == 0) {
        rc = mbedtls_sha256_update(&ctx, mac, sizeof(mac));
    }
    if (rc == 0) {
        rc = mbedtls_sha256_finish(&ctx, digest);
    }
    mbedtls_sha256_free(&ctx);
    if (rc != 0) {
        argus_password_zeroize(digest, sizeof(digest));
        return ESP_FAIL;
    }

    for (size_t i = 0U; i < ARGUS_FACTORY_CREDENTIAL_LEN; i++) {
        out[i] = ALPHABET[digest[i] % ALPHABET_SIZE];
    }
    out[ARGUS_FACTORY_CREDENTIAL_LEN] = '\0';
    argus_password_zeroize(digest, sizeof(digest));
    return ESP_OK;
}

esp_err_t argus_factory_credential_bootstrap(void)
{
    argus_password_verifier_t existing = {0};
    esp_err_t err = argus_security_store_get_console_verifier(&existing, NULL);
    argus_password_zeroize(&existing, sizeof(existing));
    if (err == ESP_OK) {
        // Already provisioned - possibly by an operator, possibly by an
        // earlier boot. Either way this function must never overwrite it.
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    char password[ARGUS_FACTORY_CREDENTIAL_LEN + 1] = {0};
    err = argus_factory_credential_derive(password, sizeof(password));
    if (err != ESP_OK) {
        argus_password_zeroize(password, sizeof(password));
        return err;
    }

    argus_password_verifier_t created = {0};
    err = argus_auth_service_create_verifier(
        (const uint8_t *)password, strlen(password), &created);
    if (err == ESP_OK) {
        err = argus_security_store_set_console_verifier(&created, false);
    }
    argus_password_zeroize(&created, sizeof(created));
    argus_password_zeroize(password, sizeof(password));

    if (err == ESP_OK) {
        // The password itself is NEVER logged. An operator recovers it with
        // tools/factory_credential.py from the MAC and the same salt.
        ESP_LOGW(TAG, "Console verifier provisioned from the per-device "
                      "FACTORY default. Change it: it is derived, not "
                      "chosen, and every tool that knows the salt can "
                      "recompute it.");
        // A verifier now exists, so the deferred migration state is stale.
        (void)argus_security_store_set_migration_state(
            ARGUS_SECURITY_MIGRATION_COMPLETE);
    }
    argus_factory_credential_refresh();
    return err;
}

void argus_factory_credential_refresh(void)
{
    s_factory_checked = false;
    s_factory_in_use = false;
}

bool argus_factory_credential_in_use(void)
{
    if (s_factory_checked) {
        return s_factory_in_use;
    }
    argus_password_verifier_t stored = {0};
    if (argus_security_store_get_console_verifier(&stored, NULL) != ESP_OK) {
        argus_password_zeroize(&stored, sizeof(stored));
        s_factory_checked = true;
        s_factory_in_use = false;
        return false;
    }
    char password[ARGUS_FACTORY_CREDENTIAL_LEN + 1] = {0};
    bool match = false;
    if (argus_factory_credential_derive(password, sizeof(password)) == ESP_OK) {
        (void)argus_password_verifier_verify(
            (const uint8_t *)password, strlen(password), &stored, &match);
    }
    argus_password_zeroize(password, sizeof(password));
    argus_password_zeroize(&stored, sizeof(stored));
    s_factory_in_use = match;
    s_factory_checked = true;
    if (match) {
        ESP_LOGW(TAG, "This controller is still using its FACTORY portal "
                      "password.");
    }
    return s_factory_in_use;
}
