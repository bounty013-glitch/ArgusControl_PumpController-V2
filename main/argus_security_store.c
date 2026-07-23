#include "argus_security_store.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define ARGUS_SECURITY_NAMESPACE "argus_sec"
#define ARGUS_SECURITY_SLOT_A_KEY "manifest_a"
#define ARGUS_SECURITY_SLOT_B_KEY "manifest_b"
#define ARGUS_SECURITY_SELECTOR_KEY "manifest_sel"
#define ARGUS_SECURITY_WRITE_QUEUE_LENGTH 4U
#define ARGUS_SECURITY_WRITER_STACK 6144U
#define ARGUS_SECURITY_WRITER_PRIORITY 3U

typedef struct {
    argus_security_payload_t payload;
    uint32_t expected_generation;
    SemaphoreHandle_t completion;
    StaticSemaphore_t completion_storage;
    esp_err_t result;
} argus_security_write_request_t;

static const char *TAG = "argus_sec_store";
static argus_security_store_core_t s_writer_core;
static argus_security_store_core_t s_snapshot;
static SemaphoreHandle_t s_snapshot_mutex;
static StaticSemaphore_t s_snapshot_mutex_storage;
static QueueHandle_t s_write_queue;
static TaskHandle_t s_writer_task;
static bool s_initialized;
static bool s_encryption_enabled;

_Static_assert(sizeof(argus_security_payload_t) <= 2048U,
               "Security manifest exceeded its bounded design size");
_Static_assert(sizeof(argus_security_payload_t) == 562U,
               "Provisioning schema packing changed; update the offline tool");
_Static_assert(sizeof(argus_security_slot_t) == 582U,
               "Provisioning slot packing changed; update the offline tool");
_Static_assert(ARGUS_SECURITY_MAX_ROLES <= 16U,
               "Role mask supports at most 16 roles");

static uint32_t crc32_bytes(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U
                      ? (crc >> 1U) ^ UINT32_C(0xedb88320)
                      : crc >> 1U;
        }
    }
    return ~crc;
}

uint32_t argus_security_payload_crc32(
    const argus_security_payload_t *payload)
{
    return payload == NULL
               ? 0U
               : crc32_bytes((const uint8_t *)payload, sizeof(*payload));
}

static bool bounded_string_valid(const char *value, size_t capacity,
                                 bool allow_empty)
{
    if (value == NULL || capacity == 0U) return false;
    size_t length = strnlen(value, capacity);
    if (length == capacity || (!allow_empty && length == 0U)) return false;
    for (size_t i = 0U; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (!(isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

static bool bounded_printable_valid(const char *value, size_t capacity,
                                    bool allow_empty)
{
    if (value == NULL || capacity == 0U) return false;
    size_t length = strnlen(value, capacity);
    if (length == capacity || (!allow_empty && length == 0U)) return false;
    for (size_t i = 0U; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20U || c > 0x7eU) return false;
    }
    return true;
}

bool argus_security_role_record_valid(
    const argus_security_role_record_t *record)
{
    if (record == NULL ||
        record->record_version != ARGUS_SECURITY_RECORD_VERSION ||
        record->level >= ARGUS_SECURITY_LEVEL_COUNT ||
        record->builtin > 1U || record->protected_role > 1U ||
        !bounded_string_valid(record->identifier,
                              sizeof(record->identifier), false) ||
        (record->permissions & ~ARGUS_PERMISSION_DEFINED_MASK) != 0U ||
        (record->delegable_permissions & ~record->permissions) != 0U) {
        return false;
    }
    if ((record->delegable_permissions &
         ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) != 0U) {
        return false;
    }
    return true;
}

bool argus_security_human_record_valid(
    const argus_security_human_record_t *record)
{
    return record != NULL &&
           record->record_version == ARGUS_SECURITY_RECORD_VERSION &&
           record->level <= ARGUS_SECURITY_LEVEL_VIEWER &&
           record->enabled <= 1U && record->protected_identity <= 1U &&
           bounded_string_valid(record->identifier,
                                sizeof(record->identifier), false) &&
           bounded_string_valid(record->login, sizeof(record->login), false) &&
           bounded_printable_valid(record->display_name,
                                   sizeof(record->display_name), false) &&
           bounded_string_valid(record->scope, sizeof(record->scope), false) &&
           record->role_mask != 0U && record->credential_version != 0U &&
           record->record_security_epoch != 0U && record->revoked <= 1U &&
           record->reserved[0] == 0U && record->reserved[1] == 0U &&
           record->reserved[2] == 0U &&
           !(record->enabled != 0U && record->revoked != 0U) &&
           (record->direct_permissions & ~ARGUS_PERMISSION_DEFINED_MASK) == 0U &&
           argus_password_verifier_record_valid(&record->verifier);
}

bool argus_security_machine_record_valid(
    const argus_security_machine_record_t *record)
{
    return record != NULL &&
           record->record_version == ARGUS_SECURITY_RECORD_VERSION &&
           record->enabled <= 1U && record->revoked <= 1U &&
           !(record->enabled != 0U && record->revoked != 0U) &&
           record->client_type >= ARGUS_MACHINE_CLIENT_HMI &&
           record->client_type <= ARGUS_MACHINE_CLIENT_TYPE_MAX &&
           record->allowed_transports != 0U &&
           (record->allowed_transports &
            ~ARGUS_MACHINE_TRANSPORT_DEFINED_MASK) == 0U &&
           record->reserved == 0U &&
           bounded_string_valid(record->identifier,
                                sizeof(record->identifier), false) &&
           bounded_printable_valid(record->display_name,
                                   sizeof(record->display_name), false) &&
           bounded_string_valid(record->scope, sizeof(record->scope), false) &&
           bounded_printable_valid(record->topic_scope,
                                   sizeof(record->topic_scope), true) &&
           bounded_printable_valid(record->api_scope,
                                   sizeof(record->api_scope), true) &&
           bounded_string_valid(record->enrollment_actor,
                                sizeof(record->enrollment_actor), false) &&
           (record->permissions & ~ARGUS_PERMISSION_DEFINED_MASK) == 0U &&
           record->credential_version != 0U &&
           record->record_security_epoch != 0U &&
           argus_password_verifier_record_valid(&record->verifier);
}

static bool ap_secret_valid(const argus_security_ap_secret_record_t *record)
{
    if (record == NULL ||
        record->record_version != ARGUS_SECURITY_RECORD_VERSION ||
        record->provisioned > 1U || record->reserved != 0U) {
        return false;
    }
    if (record->provisioned == 0U) {
        return record->length == 0U && record->credential_version == 0U;
    }
    if (record->length < ARGUS_SECURITY_AP_SECRET_MIN ||
        record->length > ARGUS_SECURITY_AP_SECRET_MAX ||
        record->credential_version == 0U ||
        record->value[record->length] != 0U) {
        return false;
    }
    for (uint8_t i = 0U; i < record->length; ++i) {
        if (record->value[i] < 0x20U || record->value[i] > 0x7eU) {
            return false;
        }
    }
    return true;
}

static argus_permission_set_t permissions_for_level(
    argus_security_level_t level)
{
    const argus_permission_set_t operational =
        ARGUS_PERMISSION_VIEW_STATUS | ARGUS_PERMISSION_REQUEST_AUTHORITY |
        ARGUS_PERMISSION_MOTION | ARGUS_PERMISSION_SOFTWARE_ESTOP |
        ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP |
        ARGUS_PERMISSION_ACK_ALARMS;
    switch (level) {
        case ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL:
            return ARGUS_PERMISSION_DEFINED_MASK;
        case ARGUS_SECURITY_LEVEL_CLIENT_ADMIN:
            return operational | ARGUS_PERMISSION_MANAGE_USERS |
                   ARGUS_PERMISSION_MANAGE_ROLES |
                   ARGUS_PERMISSION_ENROLL_MACHINES |
                   ARGUS_PERMISSION_REVOKE_MACHINES |
                   ARGUS_PERMISSION_VIEW_AUDIT;
        case ARGUS_SECURITY_LEVEL_SUPERVISOR:
        case ARGUS_SECURITY_LEVEL_OPERATOR:
            return operational;
        case ARGUS_SECURITY_LEVEL_VIEWER:
            return ARGUS_PERMISSION_VIEW_STATUS;
        case ARGUS_SECURITY_LEVEL_MACHINE:
            return 0U;
        default:
            return 0U;
    }
}

void argus_security_store_default_payload(argus_security_payload_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->schema_version = ARGUS_SECURITY_SCHEMA_VERSION;
    out->security_epoch = 1U;
    out->role_count = ARGUS_SECURITY_BUILTIN_ROLE_COUNT;
    static const char *const identifiers[ARGUS_SECURITY_BUILTIN_ROLE_COUNT] = {
        "argus_personnel", "client_admin", "supervisor",
        "operator", "viewer", "machine_identity",
    };
    for (uint8_t i = 0U; i < ARGUS_SECURITY_BUILTIN_ROLE_COUNT; ++i) {
        argus_security_role_record_t *role = &out->builtin_roles[i];
        role->record_version = ARGUS_SECURITY_RECORD_VERSION;
        role->level = i;
        role->builtin = 1U;
        role->protected_role =
            i == ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL ? 1U : 0U;
        strlcpy(role->identifier, identifiers[i], sizeof(role->identifier));
        role->permissions = permissions_for_level((argus_security_level_t)i);
        role->delegable_permissions = role->permissions &
            ~(ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS |
              ARGUS_PERMISSION_MODIFY_IDENTITY |
              ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG |
              ARGUS_PERMISSION_COMMISSION |
              ARGUS_PERMISSION_CALIBRATE |
              ARGUS_PERMISSION_MANAGE_FIRMWARE |
              ARGUS_PERMISSION_FULL_SECURITY_RESET);
    }
    out->factory_ap.record_version = ARGUS_SECURITY_RECORD_VERSION;
    out->active_ap.record_version = ARGUS_SECURITY_RECORD_VERSION;
    out->migration_state = ARGUS_SECURITY_MIGRATION_NOT_STARTED;
    out->recovery_state = ARGUS_SECURITY_RECOVERY_INACTIVE;
}

bool argus_security_payload_valid(const argus_security_payload_t *payload)
{
    if (payload == NULL ||
        payload->schema_version != ARGUS_SECURITY_SCHEMA_VERSION ||
        payload->flags != 0U || payload->security_epoch == 0U ||
        payload->role_count < ARGUS_SECURITY_BUILTIN_ROLE_COUNT ||
        payload->role_count > ARGUS_SECURITY_MAX_ROLES ||
        payload->human_count > ARGUS_SECURITY_MAX_HUMANS ||
        payload->machine_count > ARGUS_SECURITY_MAX_MACHINES ||
        payload->console_verifier_provisioned > 1U ||
        payload->migration_state > ARGUS_SECURITY_MIGRATION_FAILED ||
        payload->recovery_state > ARGUS_SECURITY_RECOVERY_REQUESTED ||
        payload->reserved != 0U || !ap_secret_valid(&payload->factory_ap) ||
        !ap_secret_valid(&payload->active_ap)) {
        return false;
    }
    for (uint8_t i = 0U; i < ARGUS_SECURITY_BUILTIN_ROLE_COUNT; ++i) {
        if (!argus_security_role_record_valid(&payload->builtin_roles[i]) ||
            payload->builtin_roles[i].level != i ||
            payload->builtin_roles[i].builtin != 1U) {
            return false;
        }
    }
    if (payload->console_verifier_provisioned != 0U) {
        if (payload->console_credential_version == 0U ||
            !argus_password_verifier_record_valid(
                &payload->console_verifier)) {
            return false;
        }
    } else if (payload->console_credential_version != 0U) {
        return false;
    }
    return true;
}

static bool slot_current_valid(const argus_security_slot_t *slot)
{
    return slot != NULL && slot->magic == ARGUS_SECURITY_SLOT_MAGIC &&
           slot->schema_version == ARGUS_SECURITY_SCHEMA_VERSION &&
           slot->payload_length == sizeof(slot->payload) &&
           slot->generation != 0U &&
           slot->valid_marker == ARGUS_SECURITY_VALID_MARKER &&
           slot->crc32 == argus_security_payload_crc32(&slot->payload) &&
           argus_security_payload_valid(&slot->payload);
}

static bool slot_unsupported(const argus_security_slot_t *slot)
{
    return slot != NULL && slot->magic == ARGUS_SECURITY_SLOT_MAGIC &&
           slot->schema_version != ARGUS_SECURITY_SCHEMA_VERSION;
}

esp_err_t argus_security_store_core_init(
    argus_security_store_core_t *core,
    const argus_security_store_driver_t *driver)
{
    if (core == NULL || driver == NULL || driver->read_slot == NULL ||
        driver->write_slot == NULL || driver->read_selector == NULL ||
        driver->write_selector == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(core, 0, sizeof(*core));
    core->driver = driver;

    argus_security_slot_t a = {0};
    argus_security_slot_t b = {0};
    esp_err_t a_err = driver->read_slot(driver->ctx, 0U, &a);
    esp_err_t b_err = driver->read_slot(driver->ctx, 1U, &b);
    bool a_missing = a_err == ESP_ERR_NVS_NOT_FOUND ||
                     a_err == ESP_ERR_NOT_FOUND;
    bool b_missing = b_err == ESP_ERR_NVS_NOT_FOUND ||
                     b_err == ESP_ERR_NOT_FOUND;
    if ((!a_missing && a_err != ESP_OK) || (!b_missing && b_err != ESP_OK)) {
        core->state = ARGUS_SECURITY_STORE_UNAVAILABLE;
        return ESP_OK;
    }
    bool a_valid = a_err == ESP_OK && slot_current_valid(&a);
    bool b_valid = b_err == ESP_OK && slot_current_valid(&b);
    if (!a_valid && !b_valid) {
        argus_security_store_default_payload(&core->active);
        if (a_missing && b_missing) {
            core->state = ARGUS_SECURITY_STORE_MISSING;
        } else if ((a_err == ESP_OK && slot_unsupported(&a)) ||
                   (b_err == ESP_OK && slot_unsupported(&b))) {
            core->state = ARGUS_SECURITY_STORE_UNSUPPORTED_VERSION;
        } else {
            core->state = ARGUS_SECURITY_STORE_CORRUPT;
        }
        return ESP_OK;
    }

    uint8_t selector = 0U;
    esp_err_t selector_err =
        driver->read_selector(driver->ctx, &selector);
    if (selector_err != ESP_OK || selector > 1U) {
        core->state = selector_err == ESP_ERR_NVS_NOT_FOUND ||
                              selector_err == ESP_ERR_NOT_FOUND ||
                              selector_err == ESP_OK
                          ? ARGUS_SECURITY_STORE_CORRUPT
                          : ARGUS_SECURITY_STORE_UNAVAILABLE;
        return ESP_OK;
    }
    uint8_t chosen;
    if ((selector == 0U && a_valid) || (selector == 1U && b_valid)) {
        chosen = selector;
    } else {
        uint8_t fallback = (uint8_t)(selector ^ 1U);
        bool fallback_valid = fallback == 0U ? a_valid : b_valid;
        if (!fallback_valid) {
            core->state = ARGUS_SECURITY_STORE_CORRUPT;
            return ESP_OK;
        }
        chosen = fallback;
    }
    const argus_security_slot_t *selected = chosen == 0U ? &a : &b;
    core->active = selected->payload;
    core->generation = selected->generation;
    core->active_slot = chosen;
    core->redundancy_degraded = !(a_valid && b_valid);
    core->state = core->active.factory_ap.provisioned != 0U &&
                          core->active.active_ap.provisioned != 0U
                      ? ARGUS_SECURITY_STORE_READY
                      : ARGUS_SECURITY_STORE_UNPROVISIONED;
    return ESP_OK;
}

esp_err_t argus_security_store_core_commit(
    argus_security_store_core_t *core,
    const argus_security_payload_t *payload)
{
    if (core == NULL || payload == NULL || core->driver == NULL ||
        !argus_security_payload_valid(payload) ||
        core->state == ARGUS_SECURITY_STORE_CORRUPT ||
        core->state == ARGUS_SECURITY_STORE_UNSUPPORTED_VERSION ||
        core->state == ARGUS_SECURITY_STORE_UNAVAILABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t target = core->generation == 0U ? 0U : (uint8_t)(core->active_slot ^ 1U);
    uint32_t next_generation = core->generation + 1U;
    if (next_generation == 0U) next_generation = 1U;
    argus_security_slot_t slot = {
        .magic = ARGUS_SECURITY_SLOT_MAGIC,
        .schema_version = ARGUS_SECURITY_SCHEMA_VERSION,
        .payload_length = sizeof(*payload),
        .generation = next_generation,
        .crc32 = argus_security_payload_crc32(payload),
        .valid_marker = ARGUS_SECURITY_VALID_MARKER,
        .payload = *payload,
    };
    esp_err_t err = core->driver->write_slot(core->driver->ctx, target, &slot);
    if (err != ESP_OK) return err;
    argus_security_slot_t readback = {0};
    err = core->driver->read_slot(core->driver->ctx, target, &readback);
    if (err != ESP_OK || !slot_current_valid(&readback) ||
        memcmp(&readback, &slot, sizeof(slot)) != 0) {
        return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
    }
    err = core->driver->write_selector(core->driver->ctx, target);
    if (err != ESP_OK) return err;
    core->active = *payload;
    core->generation = next_generation;
    core->active_slot = target;
    core->state = payload->factory_ap.provisioned != 0U &&
                          payload->active_ap.provisioned != 0U
                      ? ARGUS_SECURITY_STORE_READY
                      : ARGUS_SECURITY_STORE_UNPROVISIONED;
    core->redundancy_degraded = core->generation < 2U;
    return ESP_OK;
}

static esp_err_t prod_read_slot(void *ctx, uint8_t index,
                                argus_security_slot_t *out)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(
        ARGUS_SECURITY_PARTITION, ARGUS_SECURITY_NAMESPACE, NVS_READONLY,
        &handle);
    if (err != ESP_OK) return err;
    size_t size = sizeof(*out);
    err = nvs_get_blob(handle,
                       index == 0U ? ARGUS_SECURITY_SLOT_A_KEY
                                   : ARGUS_SECURITY_SLOT_B_KEY,
                       out, &size);
    nvs_close(handle);
    return err;
}

static esp_err_t prod_write_slot(void *ctx, uint8_t index,
                                 const argus_security_slot_t *slot)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(
        ARGUS_SECURITY_PARTITION, ARGUS_SECURITY_NAMESPACE, NVS_READWRITE,
        &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle,
                       index == 0U ? ARGUS_SECURITY_SLOT_A_KEY
                                   : ARGUS_SECURITY_SLOT_B_KEY,
                       slot, sizeof(*slot));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t prod_read_selector(void *ctx, uint8_t *out_selector)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(
        ARGUS_SECURITY_PARTITION, ARGUS_SECURITY_NAMESPACE, NVS_READONLY,
        &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(handle, ARGUS_SECURITY_SELECTOR_KEY, out_selector);
    nvs_close(handle);
    return err;
}

static esp_err_t prod_write_selector(void *ctx, uint8_t selector)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(
        ARGUS_SECURITY_PARTITION, ARGUS_SECURITY_NAMESPACE, NVS_READWRITE,
        &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, ARGUS_SECURITY_SELECTOR_KEY, selector);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static const argus_security_store_driver_t s_prod_driver = {
    .read_slot = prod_read_slot,
    .write_slot = prod_write_slot,
    .read_selector = prod_read_selector,
    .write_selector = prod_write_selector,
};

static bool partition_blank(const esp_partition_t *partition)
{
    uint8_t sample[256];
    if (partition == NULL) return false;
    for (size_t offset = 0U; offset < partition->size;
         offset += sizeof(sample)) {
        size_t remaining = partition->size - offset;
        size_t length = remaining < sizeof(sample) ? remaining : sizeof(sample);
        if (esp_partition_read(partition, offset, sample, length) != ESP_OK) {
            return false;
        }
        for (size_t i = 0U; i < length; ++i) {
            if (sample[i] != 0xffU) return false;
        }
    }
    return true;
}

static esp_err_t initialize_encrypted_partition(void)
{
    const esp_partition_t *keys = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,
        ARGUS_SECURITY_KEYS_PARTITION);
    const esp_partition_t *store = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
        ARGUS_SECURITY_PARTITION);
    if (keys == NULL || store == NULL) return ESP_ERR_NOT_FOUND;

    nvs_sec_cfg_t cfg = {0};
    esp_err_t err = nvs_flash_read_security_cfg(keys, &cfg);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        if (!partition_blank(store)) {
            ESP_LOGE(TAG, "Security keys missing for nonblank store; failing closed");
            argus_password_zeroize(&cfg, sizeof(cfg));
            return ESP_ERR_INVALID_STATE;
        }
        err = nvs_flash_generate_keys(keys, &cfg);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Generated software-held NVS keys for new security store");
        }
    }
    if (err == ESP_OK) {
        err = nvs_flash_secure_init_partition(ARGUS_SECURITY_PARTITION, &cfg);
    }
    argus_password_zeroize(&cfg, sizeof(cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Encrypted security-store initialization failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    s_encryption_enabled = true;
    return ESP_OK;
}

static void publish_snapshot(void)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_snapshot = s_writer_core;
    xSemaphoreGive(s_snapshot_mutex);
}

static void writer_task(void *ctx)
{
    (void)ctx;
    argus_security_write_request_t *request = NULL;
    for (;;) {
        if (xQueueReceive(s_write_queue, &request, portMAX_DELAY) != pdTRUE ||
            request == NULL) {
            continue;
        }
        if (request->expected_generation != s_writer_core.generation) {
            request->result = ESP_ERR_INVALID_STATE;
        } else {
            request->result = argus_security_store_core_commit(
                &s_writer_core, &request->payload);
            if (request->result == ESP_OK) publish_snapshot();
        }
        xSemaphoreGive(request->completion);
    }
}

static esp_err_t submit_payload(const argus_security_payload_t *payload,
                                uint32_t expected_generation)
{
    if (payload == NULL || s_write_queue == NULL || s_writer_task == NULL ||
        xTaskGetCurrentTaskHandle() == s_writer_task) {
        return ESP_ERR_INVALID_STATE;
    }
    argus_security_write_request_t request = {
        .payload = *payload,
        .expected_generation = expected_generation,
        .result = ESP_FAIL,
    };
    request.completion =
        xSemaphoreCreateBinaryStatic(&request.completion_storage);
    if (request.completion == NULL) {
        argus_password_zeroize(&request.payload, sizeof(request.payload));
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(s_write_queue, &request, pdMS_TO_TICKS(1000)) != pdTRUE) {
        argus_password_zeroize(&request.payload, sizeof(request.payload));
        return ESP_ERR_TIMEOUT;
    }
    (void)xSemaphoreTake(request.completion, portMAX_DELAY);
    argus_password_zeroize(&request.payload, sizeof(request.payload));
    return request.result;
}

esp_err_t argus_security_store_init(void)
{
    if (s_initialized) return ESP_OK;
    esp_err_t err = initialize_encrypted_partition();
    if (err != ESP_OK) return err;
    s_snapshot_mutex = xSemaphoreCreateMutexStatic(&s_snapshot_mutex_storage);
    if (s_snapshot_mutex == NULL) return ESP_ERR_NO_MEM;
    err = argus_security_store_core_init(&s_writer_core, &s_prod_driver);
    if (err != ESP_OK) return err;
    if (s_writer_core.state == ARGUS_SECURITY_STORE_MISSING) {
        argus_security_payload_t defaults;
        argus_security_store_default_payload(&defaults);
        err = argus_security_store_core_commit(&s_writer_core, &defaults);
        argus_password_zeroize(&defaults, sizeof(defaults));
        if (err != ESP_OK) return err;
    } else if (s_writer_core.state == ARGUS_SECURITY_STORE_CORRUPT ||
               s_writer_core.state == ARGUS_SECURITY_STORE_UNSUPPORTED_VERSION ||
               s_writer_core.state == ARGUS_SECURITY_STORE_UNAVAILABLE) {
        ESP_LOGE(TAG, "Security store failed closed in state %u",
                 (unsigned)s_writer_core.state);
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot = s_writer_core;
    s_write_queue = xQueueCreate(ARGUS_SECURITY_WRITE_QUEUE_LENGTH,
                                 sizeof(argus_security_write_request_t *));
    if (s_write_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(writer_task, "argus_sec_writer",
                    ARGUS_SECURITY_WRITER_STACK, NULL,
                    ARGUS_SECURITY_WRITER_PRIORITY, &s_writer_task) != pdPASS) {
        vQueueDelete(s_write_queue);
        s_write_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Encrypted security store ready (schema=%u generation=%lu)",
             ARGUS_SECURITY_SCHEMA_VERSION,
             (unsigned long)s_writer_core.generation);
    return ESP_OK;
}

static esp_err_t snapshot_payload(argus_security_payload_t *out_payload,
                                  uint32_t *out_generation)
{
    if (!s_initialized || out_payload == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    *out_payload = s_snapshot.active;
    if (out_generation != NULL) *out_generation = s_snapshot.generation;
    xSemaphoreGive(s_snapshot_mutex);
    return ESP_OK;
}

esp_err_t argus_security_store_get_status(
    argus_security_store_status_t *out_status)
{
    if (!s_initialized || out_status == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    const argus_security_payload_t *p = &s_snapshot.active;
    *out_status = (argus_security_store_status_t) {
        .state = s_snapshot.state,
        .schema_version = p->schema_version,
        .generation = s_snapshot.generation,
        .security_epoch = p->security_epoch,
        .role_count = p->role_count,
        .human_count = p->human_count,
        .machine_count = p->machine_count,
        .factory_ap_provisioned = p->factory_ap.provisioned != 0U,
        .active_ap_provisioned = p->active_ap.provisioned != 0U,
        .console_verifier_provisioned =
            p->console_verifier_provisioned != 0U,
        .migration_state =
            (argus_security_migration_state_t)p->migration_state,
        .recovery_state =
            (argus_security_recovery_state_t)p->recovery_state,
        .encryption_enabled = s_encryption_enabled,
        .key_physically_extractable = true,
        .redundancy_degraded = s_snapshot.redundancy_degraded,
    };
    xSemaphoreGive(s_snapshot_mutex);
    return ESP_OK;
}

esp_err_t argus_security_store_bootstrap_ap_secrets(
    const uint8_t *secret, size_t secret_len)
{
    if (secret == NULL || secret_len < ARGUS_SECURITY_AP_SECRET_MIN ||
        secret_len > ARGUS_SECURITY_AP_SECRET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    argus_security_payload_t payload;
    uint32_t generation;
    esp_err_t err = snapshot_payload(&payload, &generation);
    if (err != ESP_OK) return err;
    if (payload.factory_ap.provisioned != 0U ||
        payload.active_ap.provisioned != 0U) {
        argus_password_zeroize(&payload, sizeof(payload));
        return ESP_ERR_NOT_SUPPORTED;
    }
    argus_security_ap_secret_record_t *records[2] = {
        &payload.factory_ap, &payload.active_ap,
    };
    for (size_t i = 0U; i < 2U; ++i) {
        records[i]->record_version = ARGUS_SECURITY_RECORD_VERSION;
        records[i]->provisioned = 1U;
        records[i]->length = (uint8_t)secret_len;
        records[i]->credential_version = 1U;
        memcpy(records[i]->value, secret, secret_len);
        records[i]->value[secret_len] = 0U;
    }
    payload.security_epoch++;
    err = submit_payload(&payload, generation);
    argus_password_zeroize(&payload, sizeof(payload));
    return err;
}

esp_err_t argus_security_store_provision_initial(
    const uint8_t *factory_ap, size_t factory_ap_len,
    const uint8_t *active_ap, size_t active_ap_len,
    const argus_password_verifier_t *console_verifier)
{
    if (factory_ap == NULL || active_ap == NULL ||
        factory_ap_len < ARGUS_SECURITY_AP_SECRET_MIN ||
        factory_ap_len > ARGUS_SECURITY_AP_SECRET_MAX ||
        active_ap_len < ARGUS_SECURITY_AP_SECRET_MIN ||
        active_ap_len > ARGUS_SECURITY_AP_SECRET_MAX ||
        !argus_password_verifier_record_valid(console_verifier)) {
        return ESP_ERR_INVALID_ARG;
    }
    argus_security_payload_t payload;
    uint32_t generation;
    esp_err_t err = snapshot_payload(&payload, &generation);
    if (err != ESP_OK) return err;
    if (payload.factory_ap.provisioned != 0U ||
        payload.active_ap.provisioned != 0U ||
        payload.console_verifier_provisioned != 0U) {
        argus_password_zeroize(&payload, sizeof(payload));
        return ESP_ERR_NOT_SUPPORTED;
    }
    argus_security_ap_secret_record_t *factory = &payload.factory_ap;
    argus_security_ap_secret_record_t *active = &payload.active_ap;
    factory->record_version = ARGUS_SECURITY_RECORD_VERSION;
    factory->provisioned = 1U;
    factory->length = (uint8_t)factory_ap_len;
    factory->credential_version = 1U;
    memcpy(factory->value, factory_ap, factory_ap_len);
    factory->value[factory_ap_len] = 0U;
    active->record_version = ARGUS_SECURITY_RECORD_VERSION;
    active->provisioned = 1U;
    active->length = (uint8_t)active_ap_len;
    active->credential_version = 1U;
    memcpy(active->value, active_ap, active_ap_len);
    active->value[active_ap_len] = 0U;
    payload.console_verifier = *console_verifier;
    payload.console_verifier_provisioned = 1U;
    payload.console_credential_version = 1U;
    payload.migration_state = ARGUS_SECURITY_MIGRATION_COMPLETE;
    payload.security_epoch++;
    err = submit_payload(&payload, generation);
    argus_password_zeroize(&payload, sizeof(payload));
    return err;
}

static esp_err_t get_ap_secret(bool factory, uint8_t *out, size_t out_size,
                               size_t *out_len)
{
    if (!s_initialized || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    const argus_security_ap_secret_record_t *record =
        factory ? &s_snapshot.active.factory_ap : &s_snapshot.active.active_ap;
    esp_err_t err = ESP_OK;
    if (record->provisioned == 0U) {
        err = ESP_ERR_NOT_FOUND;
    } else if (out_size <= record->length) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        memcpy(out, record->value, record->length);
        out[record->length] = 0U;
        *out_len = record->length;
    }
    xSemaphoreGive(s_snapshot_mutex);
    return err;
}

esp_err_t argus_security_store_get_factory_ap_secret(
    uint8_t *out, size_t out_size, size_t *out_len)
{
    return get_ap_secret(true, out, out_size, out_len);
}

esp_err_t argus_security_store_get_active_ap_secret(
    uint8_t *out, size_t out_size, size_t *out_len)
{
    return get_ap_secret(false, out, out_size, out_len);
}

esp_err_t argus_security_store_set_console_verifier(
    const argus_password_verifier_t *record, bool allow_replace)
{
    if (!argus_password_verifier_record_valid(record)) {
        return ESP_ERR_INVALID_ARG;
    }
    argus_security_payload_t payload;
    uint32_t generation;
    esp_err_t err = snapshot_payload(&payload, &generation);
    if (err != ESP_OK) return err;
    if (payload.console_verifier_provisioned != 0U && !allow_replace) {
        argus_password_zeroize(&payload, sizeof(payload));
        return ESP_ERR_NOT_SUPPORTED;
    }
    payload.console_verifier = *record;
    payload.console_verifier_provisioned = 1U;
    payload.console_credential_version++;
    if (payload.console_credential_version == 0U) {
        payload.console_credential_version = 1U;
    }
    payload.security_epoch++;
    err = submit_payload(&payload, generation);
    argus_password_zeroize(&payload, sizeof(payload));
    return err;
}

esp_err_t argus_security_store_get_console_verifier(
    argus_password_verifier_t *out_record, uint32_t *out_version)
{
    if (!s_initialized || out_record == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_snapshot.active.console_verifier_provisioned == 0U) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        *out_record = s_snapshot.active.console_verifier;
        if (out_version != NULL) {
            *out_version = s_snapshot.active.console_credential_version;
        }
    }
    xSemaphoreGive(s_snapshot_mutex);
    return err;
}

static esp_err_t update_state(uint8_t migration, uint8_t recovery,
                              bool change_migration, bool change_recovery)
{
    argus_security_payload_t payload;
    uint32_t generation;
    esp_err_t err = snapshot_payload(&payload, &generation);
    if (err != ESP_OK) return err;
    if ((!change_migration || payload.migration_state == migration) &&
        (!change_recovery || payload.recovery_state == recovery)) {
        argus_password_zeroize(&payload, sizeof(payload));
        return ESP_OK;
    }
    if (change_migration) payload.migration_state = migration;
    if (change_recovery) payload.recovery_state = recovery;
    payload.security_epoch++;
    err = submit_payload(&payload, generation);
    argus_password_zeroize(&payload, sizeof(payload));
    return err;
}

esp_err_t argus_security_store_set_migration_state(
    argus_security_migration_state_t state)
{
    if (state > ARGUS_SECURITY_MIGRATION_FAILED) return ESP_ERR_INVALID_ARG;
    return update_state((uint8_t)state, 0U, true, false);
}

esp_err_t argus_security_store_set_recovery_state(
    argus_security_recovery_state_t state)
{
    if (state > ARGUS_SECURITY_RECOVERY_REQUESTED) return ESP_ERR_INVALID_ARG;
    return update_state(0U, (uint8_t)state, false, true);
}

argus_security_recovery_state_t argus_security_store_get_recovery_state(void)
{
    if (!s_initialized) return ARGUS_SECURITY_RECOVERY_INACTIVE;
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    argus_security_recovery_state_t state =
        (argus_security_recovery_state_t)s_snapshot.active.recovery_state;
    xSemaphoreGive(s_snapshot_mutex);
    return state;
}
