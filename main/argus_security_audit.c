#include "argus_security_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define AUDIT_PARTITION "sec_audit"
#define AUDIT_NAMESPACE "audit"
#define AUDIT_META_KEY "meta"
#define AUDIT_SCHEMA 1U
#define AUDIT_PHYSICAL_SLOTS (ARGUS_AUDIT_CAPACITY + 1U)
#define AUDIT_QUEUE_LENGTH 8U
#define AUDIT_TASK_STACK 5120U
#define AUDIT_TASK_PRIORITY 3U

typedef struct {
    uint16_t schema_version;
    uint16_t reserved;
    uint32_t count;
    uint32_t oldest_slot;
    uint32_t spare_slot;
    uint32_t overwritten;
    uint64_t next_sequence;
    uint32_t crc32;
} audit_meta_t;

typedef struct {
    argus_security_audit_record_t record;
    bool required;
    SemaphoreHandle_t completion;
    StaticSemaphore_t completion_storage;
    esp_err_t result;
} audit_request_t;

static audit_meta_t s_meta;
static uint64_t s_boot_id;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static bool s_available;
static bool s_finalization_degraded;
static const char *TAG = "argus_security_audit";
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
static bool s_test_fail_next_required;
#endif

_Static_assert(sizeof(argus_security_audit_record_t) <= 192U,
               "Audit record exceeded bounded design");

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

static uint32_t meta_crc(const audit_meta_t *meta)
{
    audit_meta_t copy = *meta;
    copy.crc32 = 0U;
    return crc32_bytes((const uint8_t *)&copy, sizeof(copy));
}

static uint32_t record_crc(const argus_security_audit_record_t *record)
{
    argus_security_audit_record_t copy = *record;
    copy.crc32 = 0U;
    return crc32_bytes((const uint8_t *)&copy, sizeof(copy));
}

static bool safe_text(const char *value, size_t capacity)
{
    if (value == NULL || strnlen(value, capacity) >= capacity) return false;
    for (const unsigned char *p = (const unsigned char *)value;
         *p != 0U; ++p) {
        if (*p < 0x20U || *p > 0x7eU) return false;
    }
    return true;
}

static void record_key(uint32_t slot, char out[12])
{
    snprintf(out, 12U, "e%08lx", (unsigned long)slot);
}

static esp_err_t open_audit(nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open_from_partition(
        AUDIT_PARTITION, AUDIT_NAMESPACE, mode, out);
}

static esp_err_t write_record_and_meta(
    uint32_t slot, const argus_security_audit_record_t *record,
    const audit_meta_t *next)
{
    nvs_handle_t handle;
    esp_err_t err = open_audit(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    char key[12];
    record_key(slot, key);
    err = nvs_set_blob(handle, key, record, sizeof(*record));
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, AUDIT_META_KEY, next, sizeof(*next));
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t append_locked(argus_security_audit_record_t *record)
{
    record->schema_version = AUDIT_SCHEMA;
    record->sequence = s_meta.next_sequence;
    if (record->outcome == ARGUS_AUDIT_OUTCOME_PREPARED &&
        record->lifecycle_id == 0U) {
        uint64_t folded = record->sequence ^ (record->sequence >> 16U) ^
                          (record->sequence >> 32U) ^
                          (record->sequence >> 48U);
        record->lifecycle_id = (uint16_t)folded;
        if (record->lifecycle_id == 0U) record->lifecycle_id = 1U;
    }
    record->boot_id = s_boot_id;
    record->uptime_us = (uint64_t)esp_timer_get_time();
    record->crc32 = record_crc(record);
    audit_meta_t next = s_meta;
    uint32_t target = s_meta.spare_slot;
    if (s_meta.count < ARGUS_AUDIT_CAPACITY) {
        next.count++;
        next.spare_slot = (target + 1U) % AUDIT_PHYSICAL_SLOTS;
    } else {
        next.oldest_slot =
            (s_meta.oldest_slot + 1U) % AUDIT_PHYSICAL_SLOTS;
        next.spare_slot = s_meta.oldest_slot;
        next.overwritten++;
    }
    next.next_sequence++;
    if (next.next_sequence == 0U) next.next_sequence = 1U;
    next.crc32 = meta_crc(&next);
    esp_err_t err = write_record_and_meta(target, record, &next);
    if (err == ESP_OK) s_meta = next;
    return err;
}

static void audit_task(void *ctx)
{
    (void)ctx;
    audit_request_t *request = NULL;
    for (;;) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE ||
            request == NULL) {
            continue;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        request->result = s_available
                              ? append_locked(&request->record)
                              : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_mutex);
        if (request->required) {
            xSemaphoreGive(request->completion);
        } else {
            memset(request, 0, sizeof(*request));
            free(request);
        }
    }
}

static esp_err_t append_record(
    argus_audit_event_type_t type,
    argus_audit_outcome_t outcome,
    uint8_t principal_type,
    uint16_t lifecycle_id,
    const char *actor,
    const char *target,
    const char *source,
    const char *reason,
    uint32_t security_epoch,
    bool required,
    argus_security_audit_record_t *written)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    if (required && s_test_fail_next_required) {
        s_test_fail_next_required = false;
        return ESP_FAIL;
    }
#endif
    if (!s_available || type < ARGUS_AUDIT_LOGIN_SUCCESS ||
        type > ARGUS_AUDIT_STORAGE_FAILURE ||
        outcome < ARGUS_AUDIT_OUTCOME_SUCCESS ||
        outcome > ARGUS_AUDIT_OUTCOME_PREPARED ||
        !safe_text(actor, ARGUS_AUDIT_TEXT_MAX + 1U) ||
        !safe_text(target, ARGUS_AUDIT_TEXT_MAX + 1U) ||
        !safe_text(source, 16U) ||
        !safe_text(reason, ARGUS_AUDIT_TEXT_MAX + 1U)) {
        return ESP_ERR_INVALID_ARG;
    }
    audit_request_t *request = calloc(1U, sizeof(*request));
    if (request == NULL) return ESP_ERR_NO_MEM;
    request->record.event_type = (uint16_t)type;
    request->record.outcome = (uint8_t)outcome;
    request->record.principal_type = principal_type;
    request->record.lifecycle_id = lifecycle_id;
    request->record.security_epoch = security_epoch;
    strlcpy(request->record.actor, actor, sizeof(request->record.actor));
    strlcpy(request->record.target, target, sizeof(request->record.target));
    strlcpy(request->record.source, source, sizeof(request->record.source));
    strlcpy(request->record.reason, reason, sizeof(request->record.reason));
    request->required = required;
    if (required) {
        request->completion =
            xSemaphoreCreateBinaryStatic(&request->completion_storage);
        if (request->completion == NULL) {
            free(request);
            return ESP_ERR_NO_MEM;
        }
    }
    audit_request_t *pointer = request;
    if (xQueueSend(s_queue, &pointer, 0U) != pdTRUE) {
        memset(request, 0, sizeof(*request));
        free(request);
        return ESP_ERR_NO_MEM;
    }
    if (!required) return ESP_OK;
    (void)xSemaphoreTake(request->completion, portMAX_DELAY);
    esp_err_t result = request->result;
    if (result == ESP_OK && written != NULL) *written = request->record;
    memset(request, 0, sizeof(*request));
    free(request);
    return result;
}

static bool audit_finalization_degraded(void)
{
    bool degraded;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    degraded = s_finalization_degraded;
    xSemaphoreGive(s_mutex);
    return degraded;
}

static void mark_finalization_degraded(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_finalization_degraded = true;
    xSemaphoreGive(s_mutex);
}

static esp_err_t read_slot_locked(
    uint32_t offset_from_oldest,
    argus_security_audit_record_t *out)
{
    uint32_t slot =
        (s_meta.oldest_slot + offset_from_oldest) % AUDIT_PHYSICAL_SLOTS;
    nvs_handle_t handle;
    esp_err_t err = open_audit(NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    char key[12];
    record_key(slot, key);
    size_t length = sizeof(*out);
    err = nvs_get_blob(handle, key, out, &length);
    nvs_close(handle);
    if (err == ESP_OK && length != sizeof(*out)) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (err == ESP_OK && out->schema_version != AUDIT_SCHEMA) {
        err = ESP_ERR_INVALID_VERSION;
    } else if (err == ESP_OK && out->crc32 != record_crc(out)) {
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

esp_err_t argus_security_audit_init(void)
{
    if (s_task != NULL) return ESP_OK;
    esp_err_t err = nvs_flash_init_partition(AUDIT_PARTITION);
    if (err != ESP_OK) return err;
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    err = open_audit(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    size_t length = sizeof(s_meta);
    err = nvs_get_blob(handle, AUDIT_META_KEY, &s_meta, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_meta, 0, sizeof(s_meta));
        s_meta.schema_version = AUDIT_SCHEMA;
        s_meta.next_sequence = 1U;
        s_meta.spare_slot = 0U;
        s_meta.crc32 = meta_crc(&s_meta);
        err = nvs_set_blob(handle, AUDIT_META_KEY, &s_meta, sizeof(s_meta));
        if (err == ESP_OK) err = nvs_commit(handle);
    } else if (err == ESP_OK &&
               (length != sizeof(s_meta) ||
                s_meta.schema_version != AUDIT_SCHEMA ||
                s_meta.reserved != 0U ||
                s_meta.count > ARGUS_AUDIT_CAPACITY ||
                s_meta.oldest_slot >= AUDIT_PHYSICAL_SLOTS ||
                s_meta.spare_slot >= AUDIT_PHYSICAL_SLOTS ||
                s_meta.next_sequence == 0U ||
                s_meta.crc32 != meta_crc(&s_meta))) {
        err = ESP_ERR_INVALID_CRC;
    }
    nvs_close(handle);
    if (err != ESP_OK) return err;
    do {
        esp_fill_random(&s_boot_id, sizeof(s_boot_id));
    } while (s_boot_id == 0U);
    s_queue = xQueueCreate(AUDIT_QUEUE_LENGTH, sizeof(audit_request_t *));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(audit_task, "argus_audit", AUDIT_TASK_STACK, NULL,
                    AUDIT_TASK_PRIORITY, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_available = true;
    return ESP_OK;
}

esp_err_t argus_security_audit_append(
    argus_audit_event_type_t type,
    argus_audit_outcome_t outcome,
    uint8_t principal_type,
    const char *actor,
    const char *target,
    const char *source,
    const char *reason,
    uint32_t security_epoch,
    bool required)
{
    return append_record(
        type, outcome, principal_type, 0U, actor, target, source,
        reason, security_epoch, required, NULL);
}

esp_err_t argus_security_audit_mutation_begin(
    argus_audit_event_type_t type,
    uint8_t principal_type,
    const char *actor,
    const char *target,
    const char *source,
    const char *action,
    uint32_t security_epoch,
    argus_security_audit_mutation_t *out)
{
    if (out == NULL || !safe_text(action, ARGUS_AUDIT_ACTION_MAX + 1U) ||
        action[0] == '\0' || audit_finalization_degraded()) {
        return ESP_ERR_INVALID_STATE;
    }
    char reason[ARGUS_AUDIT_TEXT_MAX + 1U];
    int length = snprintf(
        reason, sizeof(reason), "%s_prepared", action);
    if (length < 0 || (size_t)length >= sizeof(reason)) {
        return ESP_ERR_INVALID_ARG;
    }
    argus_security_audit_record_t written = {0};
    esp_err_t err = append_record(
        type, ARGUS_AUDIT_OUTCOME_PREPARED, principal_type, 0U,
        actor, target, source, reason, security_epoch, true, &written);
    if (err != ESP_OK) return err;
    memset(out, 0, sizeof(*out));
    out->event_type = type;
    out->principal_type = principal_type;
    out->lifecycle_id = written.lifecycle_id;
    out->security_epoch = security_epoch;
    strlcpy(out->actor, actor, sizeof(out->actor));
    strlcpy(out->target, target, sizeof(out->target));
    strlcpy(out->source, source, sizeof(out->source));
    strlcpy(out->action, action, sizeof(out->action));
    memset(&written, 0, sizeof(written));
    return ESP_OK;
}

esp_err_t argus_security_audit_mutation_finish(
    const argus_security_audit_mutation_t *mutation,
    bool succeeded)
{
    if (mutation == NULL || mutation->lifecycle_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char reason[ARGUS_AUDIT_TEXT_MAX + 1U];
    int length = snprintf(
        reason, sizeof(reason), "%s_%s", mutation->action,
        succeeded ? "succeeded" : "failed");
    if (length < 0 || (size_t)length >= sizeof(reason)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = append_record(
        mutation->event_type,
        succeeded ? ARGUS_AUDIT_OUTCOME_SUCCESS
                  : ARGUS_AUDIT_OUTCOME_FAILED,
        mutation->principal_type, mutation->lifecycle_id,
        mutation->actor, mutation->target, mutation->source, reason,
        mutation->security_epoch, true, NULL);
    if (err != ESP_OK) {
        mark_finalization_degraded();
        ESP_LOGE(
            TAG,
            "Privileged audit finalization failed; later mutations blocked");
    }
    return err;
}

esp_err_t argus_security_audit_get_status(
    argus_security_audit_status_t *out)
{
    if (out == NULL || s_mutex == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = (argus_security_audit_status_t) {
        .next_sequence = s_meta.next_sequence,
        .count = s_meta.count,
        .overwritten = s_meta.overwritten,
        .available = s_available,
        .finalization_degraded = s_finalization_degraded,
    };
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t argus_security_audit_read(
    uint32_t offset_from_oldest,
    argus_security_audit_record_t *out)
{
    if (out == NULL || s_mutex == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_available || offset_from_oldest >= s_meta.count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = read_slot_locked(offset_from_oldest, out);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t argus_security_audit_read_page(
    uint64_t before_sequence,
    uint32_t limit,
    argus_security_audit_page_t *out)
{
    if (out == NULL || s_mutex == NULL || limit == 0U ||
        limit > ARGUS_AUDIT_PAGE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_available ||
        (before_sequence != 0U &&
         before_sequence > s_meta.next_sequence)) {
        xSemaphoreGive(s_mutex);
        return before_sequence > s_meta.next_sequence
                   ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    uint64_t boundary =
        before_sequence == 0U ? s_meta.next_sequence : before_sequence;
    bool found_after_page = false;
    for (uint32_t reverse = 0U; reverse < s_meta.count; ++reverse) {
        uint32_t offset = s_meta.count - 1U - reverse;
        argus_security_audit_record_t record = {0};
        esp_err_t err = read_slot_locked(offset, &record);
        if (err != ESP_OK) {
            out->corruption_gap = true;
            continue;
        }
        if (record.sequence >= boundary) continue;
        if (out->count < limit) {
            out->records[out->count++] = record;
            out->next_before = record.sequence;
        } else {
            found_after_page = true;
            memset(&record, 0, sizeof(record));
            break;
        }
        memset(&record, 0, sizeof(record));
    }
    out->has_more = found_after_page;
    if (!out->has_more) out->next_before = 0U;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
void argus_security_audit_test_fail_next_required(void)
{
    s_test_fail_next_required = true;
}

void argus_security_audit_test_clear_finalization_degraded(void)
{
    if (s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_finalization_degraded = false;
    xSemaphoreGive(s_mutex);
}
#endif
