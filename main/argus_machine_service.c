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
                         ARGUS_PERMISSION_VIEW_AUDIT |
                         ARGUS_PERMISSION_MANAGE_NETWORK |
                         ARGUS_PERMISSION_CHANGE_AP_SECRET |
                         ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK |
                         ARGUS_PERMISSION_MANAGE_MQTT |
                         ARGUS_PERMISSION_MODIFY_IDENTITY |
                         ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG |
                         ARGUS_PERMISSION_COMMISSION |
                         ARGUS_PERMISSION_CALIBRATE |
                         ARGUS_PERMISSION_MANAGE_FIRMWARE |
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

esp_err_t argus_machine_service_set_permissions(
    const argus_principal_t *actor, const char *identifier,
    argus_permission_set_t permissions,
    argus_permission_set_t *out_previous)
{
    if (out_previous != NULL) *out_previous = 0U;
    // Undefined bits are refused before anything is read. A permission set
    // carrying a bit this firmware does not define cannot be authorised
    // against, so admitting it would store a capability nobody can reason
    // about.
    if ((permissions & ~ARGUS_PERMISSION_DEFINED_MASK) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    argus_machine_directory_snapshot_t *snapshot = NULL;
    size_t index;
    // Gated on ENROLL_MACHINES, not REVOKE_MACHINES: this operation GRANTS,
    // and granting is at least as privileged as creating. REVOKE_MACHINES is
    // the weaker "can take away" capability and must not become a route to
    // handing out MOTION.
    esp_err_t err = mutable_target(
        actor, identifier, ARGUS_PERMISSION_ENROLL_MACHINES,
        &snapshot, &index);
    if (err != ESP_OK) return err;
    argus_security_machine_record_t *record =
        &snapshot->payload.machines[index];

    // The same rule enrolment applies, against the target's OWN scope rather
    // than a caller-supplied one - the record already exists, so its scope is
    // a fact, not a request. actor_can_manage() carries the delegation guard
    // (nothing may be granted that the actor cannot delegate) and the
    // machine-permission ceiling (no administrative permission may ever land
    // on a machine).
    if (!actor_can_manage(actor, ARGUS_PERMISSION_ENROLL_MACHINES,
                          record->scope, permissions)) {
        snapshot_free(snapshot);
        return ESP_ERR_NOT_ALLOWED;
    }

    // A revoked record is not a permission-management target. Re-arming one
    // by editing its permissions would resurrect an identity an operator
    // deliberately retired.
    if (record->revoked != 0U) {
        snapshot_free(snapshot);
        return ESP_ERR_INVALID_STATE;
    }

    if (out_previous != NULL) *out_previous = record->permissions;

    // REPLACES rather than merges. Merge semantics would make removal
    // inexpressible and would turn an omitted field into a silent grant.
    record->permissions = permissions;
    // The revision bump is what makes this take effect on a LIVE session:
    // revalidate() compares it on every publish and subscribe, so a
    // reduction bites exactly as fast as a grant. The credential is
    // untouched - the device keeps its secret and needs no re-provisioning,
    // which is the entire point of this operation existing.
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

// Proven-source table. Separate from s_buckets on purpose: those are LRU, and
// an attacker cycling addresses evicts whatever it likes - including, if the
// reservation lived there, the very entry it is not supposed to be able to
// touch. Only a successful authentication writes here. Called under s_mutex.
typedef struct {
    uint32_t peer_key;
    uint64_t last_success_us;
    bool in_use;
} proven_source_t;

static proven_source_t s_proven[ARGUS_MACHINE_AUTH_PROVEN_SOURCES];

static bool proven_entry_live(const proven_source_t *entry, uint64_t now_us)
{
    return entry->in_use &&
           now_us - entry->last_success_us < ARGUS_MACHINE_AUTH_PROVEN_TTL_US;
}

static bool source_is_proven_locked(uint32_t peer_key, uint64_t now_us)
{
    for (size_t i = 0U; i < ARGUS_MACHINE_AUTH_PROVEN_SOURCES; ++i) {
        if (s_proven[i].peer_key == peer_key &&
            proven_entry_live(&s_proven[i], now_us)) {
            return true;
        }
    }
    return false;
}

static void mark_source_proven_locked(uint32_t peer_key, uint64_t now_us)
{
    proven_source_t *target = NULL;
    for (size_t i = 0U; i < ARGUS_MACHINE_AUTH_PROVEN_SOURCES; ++i) {
        if (s_proven[i].in_use && s_proven[i].peer_key == peer_key) {
            target = &s_proven[i];
            break;
        }
    }
    if (target == NULL) {
        // Prefer a free or expired entry; otherwise evict the oldest. Only
        // successful authentications compete for these, so eviction is
        // contention between real machines, not an attacker lever.
        target = &s_proven[0];
        for (size_t i = 0U; i < ARGUS_MACHINE_AUTH_PROVEN_SOURCES; ++i) {
            if (!proven_entry_live(&s_proven[i], now_us)) {
                target = &s_proven[i];
                break;
            }
            if (s_proven[i].last_success_us < target->last_success_us) {
                target = &s_proven[i];
            }
        }
    }
    target->in_use = true;
    target->peer_key = peer_key;
    target->last_success_us = now_us;
}

// Global machine-auth KDF token bucket. See the sizing rationale on
// ARGUS_MACHINE_AUTH_KDF_GLOBAL_BURST, and the reservation rationale - with
// its stated limits - on ARGUS_MACHINE_AUTH_PROVEN_SOURCES. Under s_mutex.
static uint64_t s_kdf_global_window_us;
static uint32_t s_kdf_global_spent;
static uint32_t s_kdf_global_refusals;
/* Refusals inside the CURRENT window. The published condition keys on this
 * rather than on the budget merely being spent: five legitimate reconnects
 * exhaust the unproven share without anything being denied, and a condition
 * that asserted there would cry wolf on an ordinary power-up storm. Observed
 * on the production image - the first assertion of the run read "THROTTLED:
 * 0 refusals", which is not a throttle. */
static uint32_t s_kdf_window_refusals;
/* Every THROTTLED outcome, whatever refused it. Published, so a flood is a
 * reported condition rather than an unexplained inability to connect. */
static uint32_t s_throttle_refusals;

static bool kdf_global_admit(uint32_t peer_key, uint64_t now_us,
                             uint32_t *retry_after_s)
{
    if (s_kdf_global_window_us == 0U ||
        now_us - s_kdf_global_window_us >= ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US) {
        s_kdf_global_window_us = now_us;
        s_kdf_global_spent = 0U;
        s_kdf_window_refusals = 0U;
    }
    // Unproven sources cannot spend the reserved tail of the budget. The
    // absolute burst still bounds everyone, proven included, so this reserves
    // capacity without raising the ceiling on total KDF work.
    uint32_t allowance = source_is_proven_locked(peer_key, now_us)
                             ? ARGUS_MACHINE_AUTH_KDF_GLOBAL_BURST
                             : ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST;
    if (s_kdf_global_spent >= allowance) {
        uint64_t remaining = ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US -
                             (now_us - s_kdf_global_window_us);
        *retry_after_s = (uint32_t)((remaining + 999999U) / 1000000U);
        s_kdf_global_refusals++;
        s_kdf_window_refusals++;
        return false;
    }
    s_kdf_global_spent++;
    return true;
}

// Exposed for the pure suite: the bound must be testable without a broker.
bool argus_machine_service_kdf_global_admit_for_test(
    uint32_t peer_key, uint64_t now_us, uint32_t *retry_after_s)
{
    uint32_t scratch = 0U;
    return kdf_global_admit(
        peer_key, now_us, retry_after_s != NULL ? retry_after_s : &scratch);
}

void argus_machine_service_kdf_global_reset_for_test(void)
{
    s_kdf_global_window_us = 0U;
    s_kdf_global_spent = 0U;
    s_kdf_global_refusals = 0U;
    s_kdf_window_refusals = 0U;
    s_throttle_refusals = 0U;
}

bool argus_machine_service_source_is_proven(uint32_t peer_key)
{
    if (s_mutex == NULL) return false;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool proven = source_is_proven_locked(peer_key, now_us);
    xSemaphoreGive(s_mutex);
    return proven;
}

void argus_machine_service_mark_source_proven_for_test(
    uint32_t peer_key, uint64_t now_us)
{
    if (s_mutex != NULL) xSemaphoreTake(s_mutex, portMAX_DELAY);
    mark_source_proven_locked(peer_key, now_us);
    if (s_mutex != NULL) xSemaphoreGive(s_mutex);
}

void argus_machine_service_clear_proven_sources_for_test(void)
{
    if (s_mutex != NULL) xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_proven, 0, sizeof(s_proven));
    if (s_mutex != NULL) xSemaphoreGive(s_mutex);
}

void argus_machine_service_get_throttle_status(
    argus_machine_auth_throttle_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->window_burst = ARGUS_MACHINE_AUTH_KDF_GLOBAL_BURST;
    out->unproven_burst = ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST;
    if (s_mutex == NULL) return;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint64_t elapsed = s_kdf_global_window_us == 0U
                           ? ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US
                           : now_us - s_kdf_global_window_us;
    bool window_open = elapsed < ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US;
    out->window_spent = window_open ? s_kdf_global_spent : 0U;
    out->refusals = s_throttle_refusals;
    out->global_refusals = s_kdf_global_refusals;

    // The published condition covers EVERY reason machine authentication is
    // currently being refused, not just the global budget.
    //
    // Measured on hardware: against a single-address CONNECT flood the
    // per-source failure lockout fires long before the global bucket does -
    // 1803 of 1840 attempts were refused by the lockout and the global bucket
    // never engaged. A condition wired only to the global bucket would have
    // stayed CLEAR through a sustained flood, which is precisely the
    // unexplained-failure experience this topic exists to prevent. The global
    // bucket is the bound against a DISTRIBUTED flood; the lockout is the one
    // a single source meets first. An operator needs to see either.
    uint32_t blocked = 0U;
    uint64_t longest_block_us = 0U;
    for (size_t i = 0U; i < ARGUS_MACHINE_AUTH_BUCKETS; ++i) {
        if (!s_buckets[i].in_use) continue;
        if (s_buckets[i].blocked_until_us > now_us) {
            blocked++;
            uint64_t remaining = s_buckets[i].blocked_until_us - now_us;
            if (remaining > longest_block_us) longest_block_us = remaining;
        }
    }
    out->blocked_sources = blocked;

    /* Refusals in the current window, not merely a spent budget - see the
     * note on s_kdf_window_refusals. */
    bool budget_refusing = window_open && s_kdf_window_refusals > 0U;
    out->throttle_active = budget_refusing || blocked > 0U;
    uint64_t clear_in_us = 0U;
    if (budget_refusing) {
        clear_in_us = ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US - elapsed;
    }
    if (longest_block_us > clear_in_us) clear_in_us = longest_block_us;
    out->seconds_until_clear =
        (uint32_t)((clear_in_us + 999999U) / 1000000U);
    for (size_t i = 0U; i < ARGUS_MACHINE_AUTH_PROVEN_SOURCES; ++i) {
        if (proven_entry_live(&s_proven[i], now_us)) out->proven_sources++;
    }
    xSemaphoreGive(s_mutex);
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
        s_throttle_refusals++;
        xSemaphoreGive(s_mutex);
        return outcome;
    }
    // Order, stated accurately because the previous comment here was not:
    // the per-source FAILURE lockout runs first (it is a lockout, not a rate
    // limit - it only fires after ARGUS_MACHINE_AUTH_FAILURE_LIMIT failures,
    // and an attacker escapes it by cycling addresses), then the GLOBAL
    // bound, which is the one that actually caps total KDF work and cannot be
    // escaped by cycling addresses because it does not look at the address
    // except to decide whether the reserved share is reachable.
    if (!kdf_global_admit(peer_key, now_us, &outcome.retry_after_s)) {
        outcome.result = ARGUS_MACHINE_AUTH_THROTTLED;
        s_throttle_refusals++;
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
    bool succeeded = outcome.result == ARGUS_MACHINE_AUTH_SUCCESS;
    record_auth_result(peer_key, now_us, succeeded);
    if (succeeded) {
        // The ONLY way into the proven table. Recorded after the credential
        // check, never before it.
        mark_source_proven_locked(peer_key, now_us);
    }
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
