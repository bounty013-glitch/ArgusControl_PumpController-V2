#include "argus_auth_service.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "argus_password_verifier.h"
#include "argus_security_directory.h"
#include "argus_security_store.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    bool occupied;
    uint32_t key;
    uint32_t failures;
    uint8_t cooldown_level;
    uint64_t window_started_us;
    uint64_t cooldown_until_us;
    uint64_t last_seen_us;
} throttle_bucket_t;

static throttle_bucket_t s_peer_buckets[ARGUS_LOGIN_BUCKET_CAPACITY];
static throttle_bucket_t s_principal_buckets[ARGUS_LOGIN_BUCKET_CAPACITY];
static throttle_bucket_t s_global_bucket = {
    .occupied = true,
    .key = 1U,
};
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;
static uint8_t s_kdf_admitted;
static uint32_t s_kdf_invocations;
static uint32_t s_inflight_peers[ARGUS_LOGIN_KDF_ADMISSION_MAX];
static uint32_t s_inflight_principals[ARGUS_LOGIN_KDF_ADMISSION_MAX];

static const argus_password_verifier_t SYNTHETIC_VERIFIER = {
    .format_version = ARGUS_PASSWORD_FORMAT_VERSION,
    .algorithm = ARGUS_PASSWORD_ALGORITHM_PBKDF2_HMAC_SHA256,
    .salt_length = ARGUS_PASSWORD_SALT_SIZE,
    .verifier_length = ARGUS_PASSWORD_VERIFIER_SIZE,
    .iterations = ARGUS_PASSWORD_ITERATIONS_DEFAULT,
    .salt = {
        0x61, 0x72, 0x67, 0x75, 0x73, 0x2d, 0x34, 0x64,
        0x33, 0x2d, 0x66, 0x61, 0x6b, 0x65, 0x21, 0x21,
    },
    .verifier = {
        0xb3, 0xd6, 0xe7, 0x62, 0x80, 0xa7, 0x7e, 0x96,
        0x4c, 0x34, 0x78, 0x08, 0x31, 0x4e, 0x6e, 0x79,
        0x08, 0x0c, 0x18, 0xfc, 0x22, 0x8d, 0x78, 0xe2,
        0x21, 0xd8, 0xf9, 0x4d, 0x08, 0xad, 0xea, 0x06,
    },
};

static bool duplicate_inflight_locked(
    uint32_t peer_key, uint32_t principal_key)
{
    for (size_t i = 0U; i < ARGUS_LOGIN_KDF_ADMISSION_MAX; ++i) {
        if (s_inflight_peers[i] == peer_key ||
            s_inflight_principals[i] == principal_key) {
            return true;
        }
    }
    return false;
}

static bool reserve_inflight_locked(
    uint32_t peer_key, uint32_t principal_key)
{
    for (size_t i = 0U; i < ARGUS_LOGIN_KDF_ADMISSION_MAX; ++i) {
        if (s_inflight_peers[i] == 0U &&
            s_inflight_principals[i] == 0U) {
            s_inflight_peers[i] = peer_key;
            s_inflight_principals[i] = principal_key;
            return true;
        }
    }
    return false;
}

static void release_inflight_locked(
    uint32_t peer_key, uint32_t principal_key)
{
    for (size_t i = 0U; i < ARGUS_LOGIN_KDF_ADMISSION_MAX; ++i) {
        if (s_inflight_peers[i] == peer_key &&
            s_inflight_principals[i] == principal_key) {
            s_inflight_peers[i] = 0U;
            s_inflight_principals[i] = 0U;
            return;
        }
    }
}

bool argus_auth_canonicalize_login(
    const char *input, char out[ARGUS_LOGIN_NAME_MAX + 1U])
{
    if (input == NULL || out == NULL) return false;
    size_t length = strnlen(input, ARGUS_LOGIN_NAME_MAX + 1U);
    if (length == 0U || length > ARGUS_LOGIN_NAME_MAX ||
        input[0] == ' ' || input[length - 1U] == ' ') {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c < 0x21U || c > 0x7eU) return false;
        out[i] = (char)tolower(c);
    }
    out[length] = '\0';
    return true;
}

bool argus_auth_new_password_valid(
    const uint8_t *password, size_t length)
{
    if (password == NULL || length < 12U ||
        length > ARGUS_PASSWORD_INPUT_MAX) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        if (password[i] < 0x20U || password[i] > 0x7eU) return false;
    }
    return true;
}

static uint32_t hash_login(const char *login)
{
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)login;
         *p != 0U; ++p) {
        hash = (hash ^ *p) * UINT32_C(16777619);
    }
    return hash == 0U ? 1U : hash;
}

static throttle_bucket_t *bucket_for(
    throttle_bucket_t *buckets, uint32_t key, uint64_t now_us)
{
    size_t free_index = ARGUS_LOGIN_BUCKET_CAPACITY;
    size_t oldest = 0U;
    for (size_t i = 0U; i < ARGUS_LOGIN_BUCKET_CAPACITY; ++i) {
        if (buckets[i].occupied && buckets[i].key == key) {
            buckets[i].last_seen_us = now_us;
            return &buckets[i];
        }
        if (!buckets[i].occupied &&
            free_index == ARGUS_LOGIN_BUCKET_CAPACITY) {
            free_index = i;
        }
        if (buckets[i].last_seen_us < buckets[oldest].last_seen_us) {
            oldest = i;
        }
    }
    size_t index = free_index != ARGUS_LOGIN_BUCKET_CAPACITY
                       ? free_index
                       : oldest;
    buckets[index] = (throttle_bucket_t) {
        .occupied = true,
        .key = key,
        .last_seen_us = now_us,
    };
    return &buckets[index];
}

static uint64_t cooldown_remaining(
    throttle_bucket_t *bucket, uint64_t now_us)
{
    if (bucket->cooldown_until_us <= now_us) return 0U;
    return bucket->cooldown_until_us - now_us;
}

static uint32_t preflight_locked(
    uint32_t peer_key, uint32_t principal_key, uint64_t now_us)
{
    throttle_bucket_t *peer =
        bucket_for(s_peer_buckets, peer_key, now_us);
    throttle_bucket_t *principal =
        bucket_for(s_principal_buckets, principal_key, now_us);
    uint64_t remaining = cooldown_remaining(peer, now_us);
    uint64_t principal_remaining =
        cooldown_remaining(principal, now_us);
    if (principal_remaining > remaining) remaining = principal_remaining;
    uint64_t global_remaining =
        cooldown_remaining(&s_global_bucket, now_us);
    if (global_remaining > remaining) remaining = global_remaining;
    return remaining == 0U
               ? 0U
               : (uint32_t)((remaining + UINT64_C(999999)) /
                            UINT64_C(1000000));
}

static void record_failure_bucket(
    throttle_bucket_t *bucket, uint64_t now_us)
{
    if (bucket->window_started_us == 0U ||
        now_us - bucket->window_started_us > ARGUS_LOGIN_WINDOW_US) {
        bucket->window_started_us = now_us;
        bucket->failures = 1U;
        return;
    }
    bucket->failures++;
    if (bucket->failures < ARGUS_LOGIN_FAILURE_LIMIT) return;
    uint64_t duration = ARGUS_LOGIN_COOLDOWN_INITIAL_US;
    for (uint8_t i = 0U; i < bucket->cooldown_level; ++i) {
        if (duration >= ARGUS_LOGIN_COOLDOWN_MAX_US / 2U) {
            duration = ARGUS_LOGIN_COOLDOWN_MAX_US;
            break;
        }
        duration *= 2U;
    }
    if (duration > ARGUS_LOGIN_COOLDOWN_MAX_US) {
        duration = ARGUS_LOGIN_COOLDOWN_MAX_US;
    }
    bucket->cooldown_until_us = now_us + duration;
    if (bucket->cooldown_level < 4U) bucket->cooldown_level++;
    bucket->failures = 0U;
    bucket->window_started_us = now_us;
}

static void record_result_locked(
    uint32_t peer_key, uint32_t principal_key,
    uint64_t now_us, bool success)
{
    throttle_bucket_t *peer =
        bucket_for(s_peer_buckets, peer_key, now_us);
    throttle_bucket_t *principal =
        bucket_for(s_principal_buckets, principal_key, now_us);
    if (success) {
        peer->failures = 0U;
        peer->window_started_us = 0U;
        principal->failures = 0U;
        principal->window_started_us = 0U;
        return;
    }
    record_failure_bucket(peer, now_us);
    record_failure_bucket(principal, now_us);
    record_failure_bucket(&s_global_bucket, now_us);
}

static bool build_human_principal(
    const argus_security_human_record_t *human,
    const argus_security_directory_snapshot_t *directory,
    uint32_t security_epoch,
    argus_principal_t *out)
{
    if (human == NULL || directory == NULL || out == NULL ||
        !argus_security_human_record_valid(human) ||
        !human->enabled || human->revoked) {
        return false;
    }
    argus_permission_set_t permissions = human->direct_permissions;
    argus_permission_set_t delegable = 0U;
    for (size_t bit = 0U; bit < ARGUS_SECURITY_MAX_ROLES; ++bit) {
        if ((human->role_mask & (UINT16_C(1) << bit)) == 0U) continue;
        if (bit < ARGUS_SECURITY_BUILTIN_ROLE_COUNT) {
            argus_security_level_t level = (argus_security_level_t)bit;
            permissions |= argus_authorization_level_permissions(level);
            delegable |= argus_authorization_level_delegable(level);
        } else {
            size_t index = bit - ARGUS_SECURITY_BUILTIN_ROLE_COUNT;
            if (index >= directory->payload.custom_role_count) return false;
            permissions |=
                directory->payload.custom_roles[index].permissions;
            delegable |=
                directory->payload.custom_roles[index].delegable_permissions;
        }
    }
    permissions &= ARGUS_PERMISSION_DEFINED_MASK;
    delegable &= permissions;
    delegable &= ~ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS;
    *out = (argus_principal_t) {
        .type = ARGUS_PRINCIPAL_HUMAN,
        .level = (argus_security_level_t)human->level,
        .permissions = permissions,
        .delegable_permissions = delegable,
        .credential_version = human->credential_version,
        .security_epoch = security_epoch,
        .principal_revision = human->record_security_epoch,
        .protected_identity = human->protected_identity != 0U,
    };
    strlcpy(out->identifier, human->identifier, sizeof(out->identifier));
    strlcpy(out->scope, human->scope, sizeof(out->scope));
    return argus_authorization_principal_valid(out);
}

esp_err_t argus_auth_service_init(void)
{
    if (s_mutex != NULL) return ESP_OK;
    if (!argus_password_verifier_record_valid(&SYNTHETIC_VERIFIER)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    return s_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

argus_login_outcome_t argus_auth_service_authenticate(
    uint32_t peer_key,
    const char *username,
    const uint8_t *password,
    size_t password_len)
{
    argus_login_outcome_t outcome = {
        .result = ARGUS_LOGIN_INVALID_REQUEST,
    };
    char canonical[ARGUS_LOGIN_NAME_MAX + 1U];
    if (s_mutex == NULL || peer_key == 0U ||
        !argus_auth_canonicalize_login(username, canonical) ||
        password == NULL || password_len == 0U ||
        password_len > ARGUS_PASSWORD_INPUT_MAX) {
        return outcome;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint32_t principal_key = hash_login(canonical);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    outcome.retry_after_s =
        preflight_locked(peer_key, principal_key, now_us);
    if (outcome.retry_after_s != 0U) {
        outcome.result = ARGUS_LOGIN_THROTTLED;
        xSemaphoreGive(s_mutex);
        return outcome;
    }
    if (s_kdf_admitted >= ARGUS_LOGIN_KDF_ADMISSION_MAX ||
        duplicate_inflight_locked(peer_key, principal_key) ||
        !reserve_inflight_locked(peer_key, principal_key)) {
        outcome.result = ARGUS_LOGIN_BUSY;
        xSemaphoreGive(s_mutex);
        return outcome;
    }
    s_kdf_admitted++;
    xSemaphoreGive(s_mutex);

    argus_password_verifier_t verifier = SYNTHETIC_VERIFIER;
    argus_security_human_record_t human = {0};
    argus_security_directory_snapshot_t *directory = NULL;
    argus_security_store_status_t status = {0};
    bool known = false;
    bool usable = false;
    bool store_available = false;
    uint32_t console_version = 0U;
    if (strcmp(canonical, "argus") == 0) {
        if (argus_security_store_get_console_verifier(
                &verifier, &console_version) == ESP_OK &&
            argus_security_store_get_status(&status) == ESP_OK) {
            store_available = true;
            known = true;
            usable = true;
        }
    } else {
        directory = calloc(1U, sizeof(*directory));
        if (directory != NULL &&
            argus_security_directory_get_snapshot(directory) == ESP_OK &&
            argus_security_store_get_status(&status) == ESP_OK) {
            store_available = true;
            for (size_t i = 0U; i < ARGUS_SECURITY_MAX_HUMANS; ++i) {
                const argus_security_human_record_t *candidate =
                    &directory->payload.humans[i];
                if (!argus_security_human_record_valid(candidate) ||
                    strcmp(candidate->login, canonical) != 0) {
                    continue;
                }
                human = *candidate;
                verifier = human.verifier;
                known = true;
                usable = human.enabled != 0U && human.revoked == 0U;
                break;
            }
        }
    }
    esp_err_t verify_err;
    bool match = false;
    verify_err = argus_password_verifier_verify(
        password, password_len, &verifier, &match);
    outcome.kdf_invoked =
        verify_err != ESP_ERR_TIMEOUT &&
        verify_err != ESP_ERR_INVALID_STATE;
    if (verify_err == ESP_OK && !store_available) {
        verify_err = ESP_ERR_INVALID_STATE;
    }
    argus_password_zeroize(&verifier, sizeof(verifier));

    bool success = verify_err == ESP_OK && match && known && usable;
    if (success && strcmp(canonical, "argus") == 0) {
        outcome.principal = (argus_principal_t) {
            .type = ARGUS_PRINCIPAL_CONSOLE,
            .level = ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL,
            .permissions = ARGUS_PERMISSION_DEFINED_MASK,
            .delegable_permissions =
                argus_authorization_level_delegable(
                    ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL),
            .credential_version = console_version,
            .security_epoch = status.security_epoch,
            .principal_revision = status.security_epoch,
            .protected_identity = true,
        };
        strlcpy(outcome.principal.identifier, "argus_console",
                sizeof(outcome.principal.identifier));
        strlcpy(outcome.principal.scope, "*",
                sizeof(outcome.principal.scope));
    } else if (success &&
               !build_human_principal(
                   &human, directory, status.security_epoch,
                   &outcome.principal)) {
        success = false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_kdf_admitted--;
    release_inflight_locked(peer_key, principal_key);
    if (outcome.kdf_invoked) s_kdf_invocations++;
    if (verify_err == ESP_OK) {
        record_result_locked(peer_key, principal_key, now_us, success);
    }
    xSemaphoreGive(s_mutex);
    outcome.result = success ? ARGUS_LOGIN_SUCCESS :
                     verify_err == ESP_ERR_TIMEOUT ? ARGUS_LOGIN_BUSY :
                     verify_err == ESP_OK ? ARGUS_LOGIN_INVALID_CREDENTIALS :
                                            ARGUS_LOGIN_STORE_UNAVAILABLE;
    argus_password_zeroize(&human, sizeof(human));
    if (directory != NULL) {
        argus_password_zeroize(directory, sizeof(*directory));
        free(directory);
    }
    return outcome;
}

size_t argus_auth_service_kdf_admitted(void)
{
    if (s_mutex == NULL) return 0U;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count = s_kdf_admitted;
    xSemaphoreGive(s_mutex);
    return count;
}

uint32_t argus_auth_service_kdf_invocations(void)
{
    if (s_mutex == NULL) return 0U;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t count = s_kdf_invocations;
    xSemaphoreGive(s_mutex);
    return count;
}

esp_err_t argus_auth_service_create_verifier(
    const uint8_t *password,
    size_t password_len,
    argus_password_verifier_t *out)
{
    if (s_mutex == NULL || out == NULL ||
        !argus_auth_new_password_valid(password, password_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_kdf_admitted >= ARGUS_LOGIN_KDF_ADMISSION_MAX) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    s_kdf_admitted++;
    xSemaphoreGive(s_mutex);

    esp_err_t err = argus_password_verifier_create(
        password, password_len, ARGUS_PASSWORD_ITERATIONS_DEFAULT, out);
    bool invoked =
        err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_kdf_admitted--;
    if (invoked) s_kdf_invocations++;
    xSemaphoreGive(s_mutex);
    return err;
}
