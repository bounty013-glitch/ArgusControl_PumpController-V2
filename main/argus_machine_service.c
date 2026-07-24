#include "argus_machine_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argus_machine_directory.h"
#include "argus_password_verifier.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    uint32_t peer_key;
    uint64_t window_started_us;
    uint64_t blocked_until_us;
    uint8_t failures;
    bool in_use;
} auth_bucket_t;

static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;
static auth_bucket_t s_buckets[ARGUS_MACHINE_AUTH_BUCKETS];
static size_t s_kdf_admitted;
static argus_password_verifier_t s_synthetic_verifier;
static bool s_initialized;

static bool bounded_text(const char *value, size_t capacity, bool empty_ok)
{
    if (value == NULL || capacity == 0U) return false;
    size_t length = strnlen(value, capacity);
    if (length >= capacity || (!empty_ok && length == 0U)) return false;
    for (size_t i = 0U; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20U || c > 0x7eU) return false;
    }
    return true;
}

bool argus_machine_service_scope_contains(
    const char *actor_scope, const char *target_scope)
{
    if (actor_scope == NULL || target_scope == NULL) return false;
    if (strcmp(actor_scope, "*") == 0) return target_scope[0] != '\0';
    if (strcmp(actor_scope, target_scope) == 0) return true;
    size_t length = strlen(actor_scope);
    return length > 0U && actor_scope[length - 1U] == '*' &&
           strncmp(actor_scope, target_scope, length - 1U) == 0;
}

bool argus_machine_service_topic_scope_contains(
    const char *semantic_scope, const char *topic)
{
    if (!bounded_text(semantic_scope, ARGUS_SECURITY_TOPIC_SCOPE_MAX + 1U,
                      false) ||
        !bounded_text(topic, 160U, false)) {
        return false;
    }
    if (strcmp(semantic_scope, "*") == 0) return true;
    size_t length = strlen(semantic_scope);
    if (strncmp(semantic_scope, topic, length) != 0) return false;
    return topic[length] == '\0' || topic[length] == '/';
}

static bool enrollment_request_valid(
    const argus_machine_enrollment_request_t *request)
{
    return request != NULL &&
           bounded_text(request->display_name,
                        sizeof(request->display_name), false) &&
           request->client_type >= ARGUS_MACHINE_CLIENT_HMI &&
           request->client_type <= ARGUS_MACHINE_CLIENT_TYPE_MAX &&
           request->allowed_transports != 0U &&
           (request->allowed_transports &
            ~ARGUS_MACHINE_TRANSPORT_DEFINED_MASK) == 0U &&
           request->allowed_interfaces != 0U &&
           (request->allowed_interfaces &
            ~ARGUS_MACHINE_INTERFACE_DEFINED_MASK) == 0U &&
           argus_security_machine_scope_valid(
               request->scope, sizeof(request->scope)) &&
           bounded_text(request->topic_scope,
                        sizeof(request->topic_scope), false) &&
           strpbrk(request->topic_scope, "+#") == NULL &&
           bounded_text(request->api_scope,
                        sizeof(request->api_scope), true) &&
           (request->permissions & ~ARGUS_PERMISSION_DEFINED_MASK) == 0U;
}

static bool actor_can_manage(
    const argus_principal_t *actor, argus_permission_set_t required,
    const char *scope, argus_permission_set_t requested)
{
    return argus_authorization_principal_valid(actor) &&
           argus_authorization_require(actor, required) ==
               ARGUS_AUTHZ_ALLOW &&
           argus_machine_service_scope_contains(actor->scope, scope) &&
           (requested & ~actor->delegable_permissions) == 0U &&
           (requested & (ARGUS_PERMISSION_MANAGE_USERS |
                         ARGUS_PERMISSION_MANAGE_ROLES |
                         ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS |
                         ARGUS_PERMISSION_ENROLL_MACHINES |
                         ARGUS_PERMISSION_REVOKE_MACHINES |
                         ARGUS_PERMISSION_CHANGE_AP_SECRET |
                         ARGUS_PERMISSION_COMMISSION |
                         ARGUS_PERMISSION_INVOKE_RECOVERY |
                         ARGUS_PERMISSION_FULL_SECURITY_RESET)) == 0U;
}

bool argus_machine_service_enrollment_allowed(
    const argus_principal_t *actor,
    const argus_machine_enrollment_request_t *request)
{
    return enrollment_request_valid(request) &&
           actor_can_manage(actor, ARGUS_PERMISSION_ENROLL_MACHINES,
                            request->scope, request->permissions);
}

static void random_identifier(char out[ARGUS_SECURITY_ID_MAX + 1U])
{
    uint8_t random[15];
    esp_fill_random(random, sizeof(random));
    out[0] = 'm';
    out[1] = '-';
    for (size_t i = 0U; i < sizeof(random); ++i) {
        static const char hex[] = "0123456789abcdef";
        out[2U + i * 2U] = hex[random[i] >> 4U];
        out[3U + i * 2U] = hex[random[i] & 0x0fU];
    }
    out[ARGUS_MACHINE_ID_LENGTH] = '\0';
    argus_password_zeroize(random, sizeof(random));
}

static void base64url_secret(
    const uint8_t input[ARGUS_MACHINE_SECRET_BYTES],
    char out[ARGUS_MACHINE_SECRET_LENGTH + 1U])
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t in = 0U;
    size_t pos = 0U;
    while (in + 3U <= ARGUS_MACHINE_SECRET_BYTES) {
        uint32_t value = ((uint32_t)input[in] << 16U) |
                         ((uint32_t)input[in + 1U] << 8U) |
                         input[in + 2U];
        out[pos++] = alphabet[(value >> 18U) & 0x3fU];
        out[pos++] = alphabet[(value >> 12U) & 0x3fU];
        out[pos++] = alphabet[(value >> 6U) & 0x3fU];
        out[pos++] = alphabet[value & 0x3fU];
        in += 3U;
    }
    uint32_t value = (uint32_t)input[in] << 16U |
                     (uint32_t)input[in + 1U] << 8U;
    out[pos++] = alphabet[(value >> 18U) & 0x3fU];
    out[pos++] = alphabet[(value >> 12U) & 0x3fU];
    out[pos++] = alphabet[(value >> 6U) & 0x3fU];
    out[pos] = '\0';
}

static esp_err_t generate_credential(
    char secret[ARGUS_MACHINE_SECRET_LENGTH + 1U],
    argus_password_verifier_t *verifier)
{
    uint8_t random[ARGUS_MACHINE_SECRET_BYTES];
    esp_fill_random(random, sizeof(random));
    base64url_secret(random, secret);
    argus_password_zeroize(random, sizeof(random));
    esp_err_t err = argus_password_verifier_create(
        (const uint8_t *)secret, ARGUS_MACHINE_SECRET_LENGTH,
        ARGUS_PASSWORD_ITERATIONS_DEFAULT, verifier);
    if (err != ESP_OK) {
        argus_password_zeroize(secret,
                               ARGUS_MACHINE_SECRET_LENGTH + 1U);
    }
    return err;
}

static esp_err_t snapshot_alloc(
    argus_machine_directory_snapshot_t **out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = calloc(1U, sizeof(**out));
    if (*out == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = argus_machine_directory_get_snapshot(*out);
    if (err != ESP_OK) {
        argus_password_zeroize(*out, sizeof(**out));
        free(*out);
        *out = NULL;
    }
    return err;
}

static void snapshot_free(argus_machine_directory_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    argus_password_zeroize(snapshot, sizeof(*snapshot));
    free(snapshot);
}

void argus_machine_service_zero_credential(
    argus_machine_credential_once_t *credential)
{
    if (credential != NULL) {
        argus_password_zeroize(credential, sizeof(*credential));
    }
}

esp_err_t argus_machine_service_init(void)
{
    if (s_initialized) return ESP_OK;
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    static const uint8_t synthetic[] =
        "argus-machine-unknown-principal-synthetic-verification";
    esp_err_t err = argus_password_verifier_create(
        synthetic, sizeof(synthetic) - 1U,
        ARGUS_PASSWORD_ITERATIONS_DEFAULT, &s_synthetic_verifier);
    if (err != ESP_OK) return err;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t argus_machine_service_enroll(
    const argus_principal_t *actor,
    const argus_machine_enrollment_request_t *request,
    argus_machine_credential_once_t *out)
{
    if (!s_initialized || out == NULL ||
        !argus_machine_service_enrollment_allowed(actor, request)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    memset(out, 0, sizeof(*out));
    argus_machine_directory_snapshot_t *snapshot = NULL;
    esp_err_t err = snapshot_alloc(&snapshot);
    if (err != ESP_OK) return err;
    if (snapshot->payload.machine_count >= ARGUS_SECURITY_MAX_MACHINES) {
        snapshot_free(snapshot);
        return ESP_ERR_NO_MEM;
    }
    argus_security_machine_record_t *record =
        &snapshot->payload.machines[snapshot->payload.machine_count];
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
    bool unique = false;
    for (size_t attempt = 0U; attempt < 8U && !unique; ++attempt) {
        random_identifier(identifier);
        unique = true;
        for (size_t i = 0U; i < snapshot->payload.machine_count; ++i) {
            if (strcmp(identifier,
                       snapshot->payload.machines[i].identifier) == 0) {
                unique = false;
                break;
            }
        }
    }
    if (!unique) {
        snapshot_free(snapshot);
        return ESP_ERR_INVALID_STATE;
    }
    argus_password_verifier_t verifier;
    err = generate_credential(out->secret, &verifier);
    if (err != ESP_OK) {
        snapshot_free(snapshot);
        return err;
    }
    *record = (argus_security_machine_record_t) {
        .record_version = ARGUS_SECURITY_RECORD_VERSION,
        .enabled = 1U,
        .client_type = (uint8_t)request->client_type,
        .allowed_transports = request->allowed_transports,
        .revoked = 0U,
        .allowed_interfaces = request->allowed_interfaces,
        .permissions = request->permissions,
        .credential_version = 1U,
        .record_security_epoch = snapshot->generation + 1U,
        .principal_revision = 1U,
        .verifier = verifier,
    };
    strlcpy(record->identifier, identifier, sizeof(record->identifier));
    strlcpy(out->identifier, record->identifier, sizeof(out->identifier));
    strlcpy(record->display_name, request->display_name,
            sizeof(record->display_name));
    strlcpy(record->scope, request->scope, sizeof(record->scope));
    strlcpy(record->topic_scope, request->topic_scope,
            sizeof(record->topic_scope));
    strlcpy(record->api_scope, request->api_scope,
            sizeof(record->api_scope));
    strlcpy(record->enrollment_actor, actor->identifier,
            sizeof(record->enrollment_actor));
    snapshot->payload.machine_count++;
    err = argus_machine_directory_commit(
        &snapshot->payload, snapshot->generation);
    if (err == ESP_OK) {
        out->credential_version = record->credential_version;
        out->principal_revision = record->principal_revision;
    } else {
        argus_machine_service_zero_credential(out);
    }
    argus_password_zeroize(&verifier, sizeof(verifier));
    snapshot_free(snapshot);
    return err;
}

static esp_err_t mutable_target(
    const argus_principal_t *actor, const char *identifier,
    argus_permission_set_t required,
    argus_machine_directory_snapshot_t **snapshot, size_t *index)
{
    if (!s_initialized || identifier == NULL || snapshot == NULL ||
        index == NULL || !argus_authorization_principal_valid(actor) ||
        argus_authorization_require(actor, required) != ARGUS_AUTHZ_ALLOW) {
        return ESP_ERR_NOT_ALLOWED;
    }
    esp_err_t err = snapshot_alloc(snapshot);
    if (err != ESP_OK) return err;
    for (size_t i = 0U; i < (*snapshot)->payload.machine_count; ++i) {
        argus_security_machine_record_t *record =
            &(*snapshot)->payload.machines[i];
        if (strcmp(identifier, record->identifier) != 0) continue;
        if (!argus_machine_service_scope_contains(
                actor->scope, record->scope)) {
            snapshot_free(*snapshot);
            *snapshot = NULL;
            return ESP_ERR_NOT_ALLOWED;
        }
        *index = i;
        return ESP_OK;
    }
    snapshot_free(*snapshot);
    *snapshot = NULL;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t argus_machine_service_rotate(
    const argus_principal_t *actor, const char *identifier,
    argus_machine_credential_once_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    argus_machine_directory_snapshot_t *snapshot = NULL;
    size_t index;
    esp_err_t err = mutable_target(
        actor, identifier, ARGUS_PERMISSION_ENROLL_MACHINES,
        &snapshot, &index);
    if (err != ESP_OK) return err;
    argus_security_machine_record_t *record =
        &snapshot->payload.machines[index];
    argus_password_verifier_t verifier;
    err = generate_credential(out->secret, &verifier);
    if (err == ESP_OK) {
        record->verifier = verifier;
        record->credential_version++;
        if (record->credential_version == 0U) record->credential_version = 1U;
        record->principal_revision++;
        if (record->principal_revision == 0U) record->principal_revision = 1U;
        record->record_security_epoch = snapshot->generation + 1U;
        err = argus_machine_directory_commit(
            &snapshot->payload, snapshot->generation);
    }
    if (err == ESP_OK) {
        strlcpy(out->identifier, record->identifier,
                sizeof(out->identifier));
        out->credential_version = record->credential_version;
        out->principal_revision = record->principal_revision;
    } else {
        argus_machine_service_zero_credential(out);
    }
    argus_password_zeroize(&verifier, sizeof(verifier));
    snapshot_free(snapshot);
    return err;
}

esp_err_t argus_machine_service_set_enabled(
    const argus_principal_t *actor, const char *identifier, bool enabled)
{
    argus_machine_directory_snapshot_t *snapshot = NULL;
    size_t index;
    esp_err_t err = mutable_target(
        actor, identifier, ARGUS_PERMISSION_REVOKE_MACHINES,
        &snapshot, &index);
    if (err != ESP_OK) return err;
    argus_security_machine_record_t *record =
        &snapshot->payload.machines[index];
    if (enabled && record->revoked != 0U) {
        snapshot_free(snapshot);
        return ESP_ERR_INVALID_STATE;
    }
    record->enabled = enabled ? 1U : 0U;
    record->principal_revision++;
    if (record->principal_revision == 0U) record->principal_revision = 1U;
    record->record_security_epoch = snapshot->generation + 1U;
    err = argus_machine_directory_commit(
        &snapshot->payload, snapshot->generation);
    snapshot_free(snapshot);
    return err;
}

esp_err_t argus_machine_service_revoke(
    const argus_principal_t *actor, const char *identifier)
{
    argus_machine_directory_snapshot_t *snapshot = NULL;
    size_t index;
    esp_err_t err = mutable_target(
        actor, identifier, ARGUS_PERMISSION_REVOKE_MACHINES,
        &snapshot, &index);
    if (err != ESP_OK) return err;
    argus_security_machine_record_t *record =
        &snapshot->payload.machines[index];
    record->enabled = 0U;
    record->revoked = 1U;
    record->credential_version++;
    if (record->credential_version == 0U) record->credential_version = 1U;
    record->principal_revision++;
    if (record->principal_revision == 0U) record->principal_revision = 1U;
    record->record_security_epoch = snapshot->generation + 1U;
    err = argus_machine_directory_commit(
        &snapshot->payload, snapshot->generation);
    snapshot_free(snapshot);
    return err;
}

esp_err_t argus_machine_service_delete(
    const argus_principal_t *actor, const char *identifier)
{
    argus_machine_directory_snapshot_t *snapshot = NULL;
    size_t index;
    esp_err_t err = mutable_target(
        actor, identifier, ARGUS_PERMISSION_REVOKE_MACHINES,
        &snapshot, &index);
    if (err != ESP_OK) return err;
    if (snapshot->payload.machines[index].revoked == 0U) {
        snapshot_free(snapshot);
        return ESP_ERR_INVALID_STATE;
    }
    size_t last = snapshot->payload.machine_count - 1U;
    argus_password_zeroize(&snapshot->payload.machines[index],
                           sizeof(snapshot->payload.machines[index]));
    if (index != last) {
        snapshot->payload.machines[index] = snapshot->payload.machines[last];
        argus_password_zeroize(&snapshot->payload.machines[last],
                               sizeof(snapshot->payload.machines[last]));
    }
    snapshot->payload.machine_count--;
    err = argus_machine_directory_commit(
        &snapshot->payload, snapshot->generation);
    snapshot_free(snapshot);
    return err;
}

esp_err_t argus_machine_service_quarantine_undisclosed(
    const char *identifier)
{
    if (!s_initialized || identifier == NULL) return ESP_ERR_INVALID_ARG;
    argus_machine_directory_snapshot_t *snapshot = NULL;
    esp_err_t err = snapshot_alloc(&snapshot);
    if (err != ESP_OK) return err;
    size_t index = snapshot->payload.machine_count;
    for (size_t i = 0U; i < snapshot->payload.machine_count; ++i) {
        if (strcmp(identifier,
                   snapshot->payload.machines[i].identifier) == 0) {
            index = i;
            break;
        }
    }
    if (index == snapshot->payload.machine_count) {
        snapshot_free(snapshot);
        return ESP_ERR_NOT_FOUND;
    }
    argus_security_machine_record_t *record =
        &snapshot->payload.machines[index];
    record->enabled = 0U;
    record->principal_revision++;
    if (record->principal_revision == 0U) record->principal_revision = 1U;
    record->record_security_epoch = snapshot->generation + 1U;
    err = argus_machine_directory_commit(
        &snapshot->payload, snapshot->generation);
    snapshot_free(snapshot);
    return err;
}

static auth_bucket_t *bucket_for(uint32_t peer_key, uint64_t now_us)
{
    auth_bucket_t *oldest = &s_buckets[0];
    for (size_t i = 0U; i < ARGUS_MACHINE_AUTH_BUCKETS; ++i) {
        auth_bucket_t *bucket = &s_buckets[i];
        if (bucket->in_use && bucket->peer_key == peer_key) return bucket;
        if (!bucket->in_use) return bucket;
        if (bucket->window_started_us < oldest->window_started_us) {
            oldest = bucket;
        }
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->window_started_us = now_us;
    return oldest;
}

static bool throttle_admit(uint32_t peer_key, uint64_t now_us,
                           uint32_t *retry_after_s)
{
    auth_bucket_t *bucket = bucket_for(peer_key, now_us);
    if (!bucket->in_use) {
        bucket->in_use = true;
        bucket->peer_key = peer_key;
        bucket->window_started_us = now_us;
    }
    if (bucket->blocked_until_us > now_us) {
        *retry_after_s =
            (uint32_t)((bucket->blocked_until_us - now_us + 999999U) /
                       1000000U);
        return false;
    }
    if (now_us - bucket->window_started_us > ARGUS_MACHINE_AUTH_WINDOW_US) {
        bucket->window_started_us = now_us;
        bucket->failures = 0U;
    }
    return true;
}

static void record_auth_result(uint32_t peer_key, uint64_t now_us, bool success)
{
    auth_bucket_t *bucket = bucket_for(peer_key, now_us);
    if (success) {
        memset(bucket, 0, sizeof(*bucket));
        return;
    }
    if (++bucket->failures >= ARGUS_MACHINE_AUTH_FAILURE_LIMIT) {
        bucket->blocked_until_us = now_us + ARGUS_MACHINE_AUTH_COOLDOWN_US;
        bucket->failures = 0U;
    }
}

static void principal_from_record(
    const argus_security_machine_record_t *record,
    uint32_t directory_generation, argus_machine_principal_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->identifier, record->identifier, sizeof(out->identifier));
    strlcpy(out->scope, record->scope, sizeof(out->scope));
    strlcpy(out->topic_scope, record->topic_scope,
            sizeof(out->topic_scope));
    strlcpy(out->api_scope, record->api_scope, sizeof(out->api_scope));
    out->permissions = record->permissions;
    out->allowed_transports = record->allowed_transports;
    out->allowed_interfaces = record->allowed_interfaces;
    out->client_type = record->client_type;
    out->credential_version = record->credential_version;
    out->principal_revision = record->principal_revision;
    out->record_security_epoch = record->record_security_epoch;
    out->directory_generation = directory_generation;
}

argus_machine_auth_outcome_t argus_machine_service_authenticate(
    uint32_t peer_key, const char *client_id,
    const uint8_t *username, size_t username_len,
    const uint8_t *password, size_t password_len,
    uint8_t receiving_interface)
{
    argus_machine_auth_outcome_t outcome = {
        .result = ARGUS_MACHINE_AUTH_INVALID_REQUEST,
    };
    if (!s_initialized || client_id == NULL || username == NULL ||
        password == NULL || username_len == 0U ||
        username_len > ARGUS_SECURITY_ID_MAX ||
        password_len != ARGUS_MACHINE_SECRET_LENGTH ||
        (receiving_interface != ARGUS_MACHINE_INTERFACE_SOFTAP &&
         receiving_interface != ARGUS_MACHINE_INTERFACE_STA)) {
        return outcome;
    }
    char identifier[ARGUS_SECURITY_ID_MAX + 1U] = {0};
    memcpy(identifier, username, username_len);
    if (memchr(username, '\0', username_len) != NULL ||
        memchr(password, '\0', password_len) != NULL ||
        strcmp(client_id, identifier) != 0) {
        outcome.result = ARGUS_MACHINE_AUTH_CLIENT_ID_REJECTED;
        return outcome;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!throttle_admit(peer_key, now_us, &outcome.retry_after_s)) {
        outcome.result = ARGUS_MACHINE_AUTH_THROTTLED;
        xSemaphoreGive(s_mutex);
        return outcome;
    }
    if (s_kdf_admitted >= ARGUS_MACHINE_AUTH_KDF_ADMISSION_MAX) {
        outcome.result = ARGUS_MACHINE_AUTH_BUSY;
        xSemaphoreGive(s_mutex);
        return outcome;
    }
    s_kdf_admitted++;
    xSemaphoreGive(s_mutex);

    argus_security_machine_record_t record = {0};
    uint32_t generation = 0U;
    esp_err_t find = argus_machine_directory_find(
        identifier, &record, NULL, &generation);
    const argus_password_verifier_t *verifier =
        find == ESP_OK ? &record.verifier : &s_synthetic_verifier;
    bool match = false;
    outcome.kdf_invoked = true;
    esp_err_t verify = argus_password_verifier_verify(
        password, password_len, verifier, &match);

    if (verify != ESP_OK) {
        outcome.result = verify == ESP_ERR_TIMEOUT
                             ? ARGUS_MACHINE_AUTH_BUSY
                             : ARGUS_MACHINE_AUTH_DIRECTORY_UNAVAILABLE;
    } else if (find != ESP_OK && find != ESP_ERR_NOT_FOUND) {
        outcome.result = ARGUS_MACHINE_AUTH_DIRECTORY_UNAVAILABLE;
    } else if (find != ESP_OK || !match ||
               record.enabled == 0U || record.revoked != 0U) {
        outcome.result = ARGUS_MACHINE_AUTH_INVALID_CREDENTIALS;
    } else if ((record.allowed_transports &
                ARGUS_MACHINE_TRANSPORT_MQTT) == 0U) {
        outcome.result = ARGUS_MACHINE_AUTH_TRANSPORT_REJECTED;
    } else if ((record.allowed_interfaces & receiving_interface) == 0U) {
        outcome.result = ARGUS_MACHINE_AUTH_INTERFACE_REJECTED;
    } else {
        principal_from_record(&record, generation, &outcome.principal);
        outcome.result = ARGUS_MACHINE_AUTH_SUCCESS;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_kdf_admitted--;
    record_auth_result(
        peer_key, now_us, outcome.result == ARGUS_MACHINE_AUTH_SUCCESS);
    xSemaphoreGive(s_mutex);
    argus_password_zeroize(&record, sizeof(record));
    return outcome;
}

esp_err_t argus_machine_service_revalidate(
    const argus_machine_principal_t *principal,
    uint8_t receiving_interface)
{
    if (!s_initialized || principal == NULL ||
        principal->identifier[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    argus_security_machine_record_t current = {0};
    uint32_t generation = 0U;
    esp_err_t err = argus_machine_directory_find(
        principal->identifier, &current, NULL, &generation);
    bool valid = err == ESP_OK && current.enabled != 0U &&
                 current.revoked == 0U &&
                 current.credential_version == principal->credential_version &&
                 current.principal_revision == principal->principal_revision &&
                 current.record_security_epoch ==
                     principal->record_security_epoch &&
                 (current.allowed_transports &
                  ARGUS_MACHINE_TRANSPORT_MQTT) != 0U &&
                 (current.allowed_interfaces & receiving_interface) != 0U &&
                 current.permissions == principal->permissions &&
                 strcmp(current.scope, principal->scope) == 0 &&
                 strcmp(current.topic_scope, principal->topic_scope) == 0;
    argus_password_zeroize(&current, sizeof(current));
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

size_t argus_machine_service_kdf_admitted(void)
{
    if (!s_initialized) return 0U;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t result = s_kdf_admitted;
    xSemaphoreGive(s_mutex);
    return result;
}
