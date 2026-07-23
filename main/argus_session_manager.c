#include "argus_session_manager.h"

#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/sha256.h"

static argus_session_table_t s_sessions;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;

static void zero_record(argus_session_record_t *record)
{
    if (record != NULL) {
        volatile uint8_t *p = (volatile uint8_t *)record;
        for (size_t i = 0U; i < sizeof(*record); ++i) p[i] = 0U;
    }
}

static bool hex_decode(const char *hex, uint8_t *out, size_t length)
{
    if (hex == NULL || out == NULL ||
        strnlen(hex, length * 2U + 1U) != length * 2U) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        unsigned char hi = (unsigned char)hex[i * 2U];
        unsigned char lo = (unsigned char)hex[i * 2U + 1U];
        uint8_t a = hi >= '0' && hi <= '9' ? (uint8_t)(hi - '0') :
                    hi >= 'a' && hi <= 'f' ? (uint8_t)(hi - 'a' + 10) : 255U;
        uint8_t b = lo >= '0' && lo <= '9' ? (uint8_t)(lo - '0') :
                    lo >= 'a' && lo <= 'f' ? (uint8_t)(lo - 'a' + 10) : 255U;
        if (a == 255U || b == 255U) return false;
        out[i] = (uint8_t)((a << 4U) | b);
    }
    return true;
}

static void hex_encode(const uint8_t *input, size_t length, char *out)
{
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0U; i < length; ++i) {
        out[i * 2U] = HEX[input[i] >> 4U];
        out[i * 2U + 1U] = HEX[input[i] & 0x0fU];
    }
    out[length * 2U] = '\0';
}

static bool digest_secret(const char *secret, uint8_t out[32])
{
    uint8_t decoded[ARGUS_SESSION_SECRET_BYTES];
    if (!hex_decode(secret, decoded, sizeof(decoded))) return false;
    int rc = mbedtls_sha256(decoded, sizeof(decoded), out, 0);
    memset(decoded, 0, sizeof(decoded));
    return rc == 0;
}

static bool expired(const argus_session_record_t *record, uint64_t now_us)
{
    return now_us < record->created_us || now_us < record->last_activity_us ||
           now_us - record->created_us >= ARGUS_SESSION_ABSOLUTE_US ||
           now_us - record->last_activity_us >= ARGUS_SESSION_IDLE_US;
}

void argus_session_table_init(argus_session_table_t *table)
{
    if (table != NULL) memset(table, 0, sizeof(*table));
}

esp_err_t argus_session_create(
    argus_session_table_t *table,
    const argus_principal_t *principal,
    uint64_t now_us,
    argus_session_random_fn random_fn,
    void *random_ctx,
    argus_session_credentials_t *out)
{
    if (table == NULL || !argus_authorization_principal_valid(principal) ||
        random_fn == NULL || out == NULL || now_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    size_t free_index = ARGUS_SESSION_CAPACITY;
    size_t principal_count = 0U;
    for (size_t i = 0U; i < ARGUS_SESSION_CAPACITY; ++i) {
        argus_session_record_t *record = &table->records[i];
        if (record->occupied && expired(record, now_us)) zero_record(record);
        if (!record->occupied) {
            if (free_index == ARGUS_SESSION_CAPACITY) free_index = i;
            continue;
        }
        if (strcmp(record->principal.identifier,
                   principal->identifier) == 0) {
            principal_count++;
        }
    }
    if (principal_count >= ARGUS_SESSION_PER_PRINCIPAL ||
        free_index == ARGUS_SESSION_CAPACITY) {
        return ESP_ERR_NO_MEM;
    }
    size_t index = free_index;
    uint8_t token[ARGUS_SESSION_SECRET_BYTES] = {0};
    uint8_t csrf[ARGUS_SESSION_SECRET_BYTES] = {0};
    uint8_t token_digest[32] = {0};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    bool unique = false;
    for (size_t attempt = 0U; attempt < 4U && !unique; ++attempt) {
        err = random_fn(random_ctx, token, sizeof(token));
        if (err != ESP_OK) break;
        bool all_zero = true;
        for (size_t i = 0U; i < sizeof(token); ++i) {
            all_zero &= token[i] == 0U;
        }
        if (all_zero ||
            mbedtls_sha256(token, sizeof(token), token_digest, 0) != 0) {
            continue;
        }
        unique = true;
        for (size_t i = 0U; i < ARGUS_SESSION_CAPACITY; ++i) {
            if (table->records[i].occupied &&
                mbedtls_ct_memcmp(
                    table->records[i].token_digest, token_digest,
                    sizeof(token_digest)) == 0) {
                unique = false;
            }
        }
    }
    if (unique) err = random_fn(random_ctx, csrf, sizeof(csrf));
    bool all_zero_csrf = true;
    for (size_t i = 0U; i < sizeof(csrf); ++i) {
        all_zero_csrf &= csrf[i] == 0U;
    }
    if (err != ESP_OK || !unique || all_zero_csrf) {
        memset(token, 0, sizeof(token));
        memset(csrf, 0, sizeof(csrf));
        memset(token_digest, 0, sizeof(token_digest));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    argus_session_record_t next = {
        .occupied = true,
        .principal = *principal,
        .created_us = now_us,
        .last_activity_us = now_us,
        .reauthenticated_us = now_us,
    };
    memcpy(next.token_digest, token_digest, sizeof(next.token_digest));
    (void)mbedtls_sha256(csrf, sizeof(csrf), next.csrf_digest, 0);
    hex_encode(token, sizeof(token), out->token);
    hex_encode(csrf, sizeof(csrf), out->csrf);
    memcpy(next.csrf_secret, out->csrf, sizeof(next.csrf_secret));
    out->principal = *principal;
    out->expires_us = now_us + ARGUS_SESSION_IDLE_US;
    zero_record(&table->records[index]);
    table->records[index] = next;
    memset(token, 0, sizeof(token));
    memset(csrf, 0, sizeof(csrf));
    memset(token_digest, 0, sizeof(token_digest));
    return ESP_OK;
}

esp_err_t argus_session_authenticate(
    argus_session_table_t *table,
    const char *token,
    uint64_t now_us,
    argus_principal_t *out_principal,
    size_t *out_index)
{
    if (table == NULL || token == NULL || out_principal == NULL ||
        now_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t digest[32];
    if (!digest_secret(token, digest)) return ESP_ERR_NOT_FOUND;
    size_t match = ARGUS_SESSION_CAPACITY;
    for (size_t i = 0U; i < ARGUS_SESSION_CAPACITY; ++i) {
        argus_session_record_t *record = &table->records[i];
        if (record->occupied && expired(record, now_us)) zero_record(record);
        if (record->occupied &&
            mbedtls_ct_memcmp(record->token_digest, digest,
                              sizeof(digest)) == 0) {
            match = i;
        }
    }
    memset(digest, 0, sizeof(digest));
    if (match == ARGUS_SESSION_CAPACITY) return ESP_ERR_NOT_FOUND;
    table->records[match].last_activity_us = now_us;
    *out_principal = table->records[match].principal;
    if (out_index != NULL) *out_index = match;
    return ESP_OK;
}

bool argus_session_csrf_valid(
    const argus_session_table_t *table,
    size_t index,
    const char *csrf)
{
    if (table == NULL || index >= ARGUS_SESSION_CAPACITY ||
        !table->records[index].occupied) {
        return false;
    }
    uint8_t digest[32];
    if (!digest_secret(csrf, digest)) return false;
    bool valid = mbedtls_ct_memcmp(table->records[index].csrf_digest,
                                  digest, sizeof(digest)) == 0;
    memset(digest, 0, sizeof(digest));
    return valid;
}

bool argus_session_get_csrf(
    const argus_session_table_t *table,
    size_t index,
    char out[ARGUS_SESSION_TOKEN_HEX_LEN + 1U])
{
    if (table == NULL || out == NULL ||
        index >= ARGUS_SESSION_CAPACITY ||
        !table->records[index].occupied) {
        return false;
    }
    memcpy(out, table->records[index].csrf_secret,
           ARGUS_SESSION_TOKEN_HEX_LEN + 1U);
    return true;
}

bool argus_session_recently_reauthenticated(
    const argus_session_table_t *table,
    size_t index,
    uint64_t now_us)
{
    return table != NULL && index < ARGUS_SESSION_CAPACITY &&
           table->records[index].occupied &&
           now_us >= table->records[index].reauthenticated_us &&
           now_us - table->records[index].reauthenticated_us <
               ARGUS_SESSION_REAUTH_US;
}

void argus_session_mark_reauthenticated(
    argus_session_table_t *table,
    size_t index,
    uint64_t now_us)
{
    if (table != NULL && index < ARGUS_SESSION_CAPACITY &&
        table->records[index].occupied) {
        table->records[index].reauthenticated_us = now_us;
    }
}

size_t argus_session_revoke_principal(
    argus_session_table_t *table,
    const char *principal_id)
{
    if (table == NULL || principal_id == NULL) return 0U;
    size_t count = 0U;
    for (size_t i = 0U; i < ARGUS_SESSION_CAPACITY; ++i) {
        if (table->records[i].occupied &&
            strcmp(table->records[i].principal.identifier,
                   principal_id) == 0) {
            zero_record(&table->records[i]);
            count++;
        }
    }
    return count;
}

bool argus_session_revoke_token(
    argus_session_table_t *table,
    const char *token)
{
    if (table == NULL) return false;
    uint8_t digest[32];
    if (!digest_secret(token, digest)) return false;
    for (size_t i = 0U; i < ARGUS_SESSION_CAPACITY; ++i) {
        if (table->records[i].occupied &&
            mbedtls_ct_memcmp(table->records[i].token_digest, digest,
                              sizeof(digest)) == 0) {
            memset(digest, 0, sizeof(digest));
            zero_record(&table->records[i]);
            return true;
        }
    }
    memset(digest, 0, sizeof(digest));
    return false;
}

size_t argus_session_revoke_epoch(
    argus_session_table_t *table,
    uint32_t current_security_epoch)
{
    if (table == NULL || current_security_epoch == 0U) return 0U;
    size_t count = 0U;
    for (size_t i = 0U; i < ARGUS_SESSION_CAPACITY; ++i) {
        if (table->records[i].occupied &&
            table->records[i].principal.security_epoch !=
                current_security_epoch) {
            zero_record(&table->records[i]);
            count++;
        }
    }
    return count;
}

size_t argus_session_count(const argus_session_table_t *table)
{
    if (table == NULL) return 0U;
    size_t count = 0U;
    for (size_t i = 0U; i < ARGUS_SESSION_CAPACITY; ++i) {
        count += table->records[i].occupied ? 1U : 0U;
    }
    return count;
}

size_t argus_session_revoke_all(argus_session_table_t *table)
{
    if (table == NULL) return 0U;
    size_t count = argus_session_count(table);
    for (size_t i = 0U; i < ARGUS_SESSION_CAPACITY; ++i) {
        zero_record(&table->records[i]);
    }
    return count;
}

static esp_err_t production_random(void *ctx, uint8_t *out, size_t length)
{
    (void)ctx;
    if (out == NULL || length == 0U) return ESP_ERR_INVALID_ARG;
    esp_fill_random(out, length);
    return ESP_OK;
}

esp_err_t argus_session_manager_init(void)
{
    if (s_mutex != NULL) return ESP_OK;
    argus_session_table_init(&s_sessions);
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    return s_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t argus_session_manager_create(
    const argus_principal_t *principal,
    argus_session_credentials_t *out)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = argus_session_create(
        &s_sessions, principal, (uint64_t)esp_timer_get_time(),
        production_random, NULL, out);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t argus_session_manager_authenticate(
    const char *token,
    argus_principal_t *out_principal,
    size_t *out_index)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = argus_session_authenticate(
        &s_sessions, token, (uint64_t)esp_timer_get_time(),
        out_principal, out_index);
    xSemaphoreGive(s_mutex);
    return err;
}

bool argus_session_manager_csrf_valid(size_t index, const char *csrf)
{
    if (s_mutex == NULL) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool valid = argus_session_csrf_valid(&s_sessions, index, csrf);
    xSemaphoreGive(s_mutex);
    return valid;
}

bool argus_session_manager_get_csrf(
    size_t index,
    char out[ARGUS_SESSION_TOKEN_HEX_LEN + 1U])
{
    if (s_mutex == NULL) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool valid = argus_session_get_csrf(&s_sessions, index, out);
    xSemaphoreGive(s_mutex);
    return valid;
}

bool argus_session_manager_recently_reauthenticated(size_t index)
{
    if (s_mutex == NULL) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool result = argus_session_recently_reauthenticated(
        &s_sessions, index, (uint64_t)esp_timer_get_time());
    xSemaphoreGive(s_mutex);
    return result;
}

void argus_session_manager_mark_reauthenticated(size_t index)
{
    if (s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    argus_session_mark_reauthenticated(
        &s_sessions, index, (uint64_t)esp_timer_get_time());
    xSemaphoreGive(s_mutex);
}

bool argus_session_manager_revoke_token(const char *token)
{
    if (s_mutex == NULL) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool result = argus_session_revoke_token(&s_sessions, token);
    xSemaphoreGive(s_mutex);
    return result;
}

size_t argus_session_manager_revoke_principal(const char *principal_id)
{
    if (s_mutex == NULL) return 0U;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count =
        argus_session_revoke_principal(&s_sessions, principal_id);
    xSemaphoreGive(s_mutex);
    return count;
}

size_t argus_session_manager_revoke_epoch(uint32_t current_security_epoch)
{
    if (s_mutex == NULL) return 0U;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count =
        argus_session_revoke_epoch(&s_sessions, current_security_epoch);
    xSemaphoreGive(s_mutex);
    return count;
}

size_t argus_session_manager_count(void)
{
    if (s_mutex == NULL) return 0U;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count = argus_session_count(&s_sessions);
    xSemaphoreGive(s_mutex);
    return count;
}

size_t argus_session_manager_revoke_all(void)
{
    if (s_mutex == NULL) return 0U;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count = argus_session_revoke_all(&s_sessions);
    xSemaphoreGive(s_mutex);
    return count;
}
