#include "argus_machine_directory.h"

#include <stdlib.h>
#include <string.h>

#include "argus_password_verifier.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define MACHINE_NAMESPACE "argus_machines"
#define MACHINE_SLOT_A "machines_a"
#define MACHINE_SLOT_B "machines_b"
#define MACHINE_SELECTOR "machines_sel"
#define MACHINE_QUEUE_LENGTH 2U
#define MACHINE_WRITER_STACK 6144U
#define MACHINE_WRITER_PRIORITY 3U

typedef struct {
    argus_machine_directory_payload_t *payload;
    uint32_t expected_generation;
    SemaphoreHandle_t completion;
    StaticSemaphore_t completion_storage;
    esp_err_t result;
} machine_request_t;

static const char *TAG = "argus_machine_dir";
static argus_machine_directory_snapshot_t s_snapshot;
static argus_machine_directory_state_t s_state =
    ARGUS_MACHINE_DIRECTORY_MISSING;
static uint8_t s_active_slot;
static bool s_redundancy_degraded;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;
static QueueHandle_t s_queue;
static TaskHandle_t s_writer;
static bool s_initialized;

_Static_assert(sizeof(argus_machine_directory_payload_t) < 16384U,
               "Machine directory exceeds bounded storage design");

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

uint32_t argus_machine_directory_crc32(
    const argus_machine_directory_payload_t *payload)
{
    return payload == NULL
               ? 0U
               : crc32_bytes((const uint8_t *)payload, sizeof(*payload));
}

bool argus_machine_directory_payload_valid(
    const argus_machine_directory_payload_t *payload)
{
    if (payload == NULL ||
        payload->schema_version != ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION ||
        payload->machine_count > ARGUS_SECURITY_MAX_MACHINES ||
        payload->reserved != 0U) {
        return false;
    }
    static const argus_security_machine_record_t zero = {0};
    for (size_t i = 0U; i < ARGUS_SECURITY_MAX_MACHINES; ++i) {
        if (i >= payload->machine_count) {
            if (memcmp(&payload->machines[i], &zero, sizeof(zero)) != 0) {
                return false;
            }
            continue;
        }
        if (!argus_security_machine_record_valid(&payload->machines[i])) {
            return false;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (strcmp(payload->machines[i].identifier,
                       payload->machines[j].identifier) == 0) {
                return false;
            }
        }
    }
    return true;
}

bool argus_machine_directory_slot_valid(
    const argus_machine_directory_slot_t *slot)
{
    return slot != NULL && slot->magic == ARGUS_MACHINE_DIRECTORY_MAGIC &&
           slot->schema_version == ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION &&
           slot->payload_length == sizeof(slot->payload) &&
           slot->generation != 0U &&
           slot->valid_marker == ARGUS_MACHINE_DIRECTORY_VALID &&
           slot->crc32 == argus_machine_directory_crc32(&slot->payload) &&
           argus_machine_directory_payload_valid(&slot->payload);
}

bool argus_machine_directory_commit_precondition(
    const argus_machine_directory_payload_t *payload,
    uint32_t expected_generation, uint32_t current_generation,
    bool directory_ready)
{
    return directory_ready &&
           argus_machine_directory_payload_valid(payload) &&
           expected_generation == current_generation;
}

static bool missing_status(esp_err_t status)
{
    return status == ESP_ERR_NVS_NOT_FOUND || status == ESP_ERR_NOT_FOUND;
}

esp_err_t argus_machine_directory_select_for_test(
    const argus_machine_directory_slot_t *slot_a, esp_err_t slot_a_status,
    const argus_machine_directory_slot_t *slot_b, esp_err_t slot_b_status,
    uint8_t selector, esp_err_t selector_status,
    argus_machine_directory_snapshot_t *out, uint8_t *out_active_slot,
    bool *out_selector_repair)
{
    if (out == NULL || out_active_slot == NULL ||
        out_selector_repair == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool a_missing = missing_status(slot_a_status);
    bool b_missing = missing_status(slot_b_status);
    bool a_valid = slot_a_status == ESP_OK &&
                   argus_machine_directory_slot_valid(slot_a);
    bool b_valid = slot_b_status == ESP_OK &&
                   argus_machine_directory_slot_valid(slot_b);
    if (a_missing && b_missing) return ESP_ERR_NOT_FOUND;
    if ((!a_missing && slot_a_status != ESP_OK) ||
        (!b_missing && slot_b_status != ESP_OK)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!a_valid && !b_valid) {
        bool unsupported =
            (slot_a_status == ESP_OK && slot_a != NULL &&
             slot_a->magic == ARGUS_MACHINE_DIRECTORY_MAGIC &&
             slot_a->schema_version != ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION) ||
            (slot_b_status == ESP_OK && slot_b != NULL &&
             slot_b->magic == ARGUS_MACHINE_DIRECTORY_MAGIC &&
             slot_b->schema_version != ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION);
        return unsupported ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_CRC;
    }

    uint8_t chosen;
    bool selector_usable = selector_status == ESP_OK && selector <= 1U &&
                           ((selector == 0U && a_valid) ||
                            (selector == 1U && b_valid));
    if (selector_usable) {
        chosen = selector;
    } else if (a_valid && b_valid) {
        chosen = slot_b->generation > slot_a->generation ? 1U : 0U;
    } else {
        chosen = a_valid ? 0U : 1U;
    }
    const argus_machine_directory_slot_t *selected =
        chosen == 0U ? slot_a : slot_b;
    out->payload = selected->payload;
    out->generation = selected->generation;
    *out_active_slot = chosen;
    *out_selector_repair = !selector_usable;
    return ESP_OK;
}

static esp_err_t open_directory(nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open_from_partition(
        ARGUS_SECURITY_PARTITION, MACHINE_NAMESPACE, mode, out);
}

static esp_err_t read_slot(uint8_t index,
                           argus_machine_directory_slot_t *out)
{
    nvs_handle_t handle;
    esp_err_t err = open_directory(NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t length = sizeof(*out);
    err = nvs_get_blob(handle, index == 0U ? MACHINE_SLOT_A : MACHINE_SLOT_B,
                       out, &length);
    nvs_close(handle);
    return err;
}

static esp_err_t write_slot(uint8_t index,
                            const argus_machine_directory_slot_t *slot)
{
    nvs_handle_t handle;
    esp_err_t err = open_directory(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, index == 0U ? MACHINE_SLOT_A : MACHINE_SLOT_B,
                       slot, sizeof(*slot));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t read_selector(uint8_t *out)
{
    nvs_handle_t handle;
    esp_err_t err = open_directory(NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(handle, MACHINE_SELECTOR, out);
    nvs_close(handle);
    return err;
}

static esp_err_t write_selector(uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = open_directory(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, MACHINE_SELECTOR, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t commit_locked(
    const argus_machine_directory_payload_t *payload,
    uint32_t expected_generation)
{
    if (!argus_machine_directory_commit_precondition(
            payload, expected_generation, s_snapshot.generation,
            s_state == ARGUS_MACHINE_DIRECTORY_READY)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t target = s_snapshot.generation == 0U
                         ? 0U
                         : (uint8_t)(s_active_slot ^ 1U);
    uint32_t generation = s_snapshot.generation + 1U;
    if (generation == 0U) generation = 1U;
    argus_machine_directory_slot_t *slot = calloc(1U, sizeof(*slot));
    argus_machine_directory_slot_t *readback = calloc(1U, sizeof(*readback));
    if (slot == NULL || readback == NULL) {
        free(slot);
        free(readback);
        return ESP_ERR_NO_MEM;
    }
    *slot = (argus_machine_directory_slot_t) {
        .magic = ARGUS_MACHINE_DIRECTORY_MAGIC,
        .schema_version = ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION,
        .payload_length = sizeof(*payload),
        .generation = generation,
        .crc32 = argus_machine_directory_crc32(payload),
        .valid_marker = ARGUS_MACHINE_DIRECTORY_VALID,
        .payload = *payload,
    };
    esp_err_t err = write_slot(target, slot);
    if (err == ESP_OK) err = read_slot(target, readback);
    if (err == ESP_OK &&
        (!argus_machine_directory_slot_valid(readback) ||
         memcmp(slot, readback, sizeof(*slot)) != 0)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) err = write_selector(target);
    if (err == ESP_OK) {
        s_snapshot.payload = *payload;
        s_snapshot.generation = generation;
        s_active_slot = target;
        s_redundancy_degraded = false;
    }
    argus_password_zeroize(slot, sizeof(*slot));
    argus_password_zeroize(readback, sizeof(*readback));
    free(slot);
    free(readback);
    return err;
}

static void writer_task(void *ctx)
{
    (void)ctx;
    machine_request_t *request = NULL;
    for (;;) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE ||
            request == NULL) {
            continue;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        request->result = commit_locked(
            request->payload, request->expected_generation);
        xSemaphoreGive(s_mutex);
        xSemaphoreGive(request->completion);
    }
}

esp_err_t argus_machine_directory_init(void)
{
    if (s_initialized) return ESP_OK;
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    argus_machine_directory_slot_t *a = calloc(1U, sizeof(*a));
    argus_machine_directory_slot_t *b = calloc(1U, sizeof(*b));
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t a_status = read_slot(0U, a);
    esp_err_t b_status = read_slot(1U, b);
    uint8_t selector = 0U;
    esp_err_t selector_status = read_selector(&selector);
    bool repair = false;
    esp_err_t err = argus_machine_directory_select_for_test(
        a, a_status, b, b_status, selector, selector_status,
        &s_snapshot, &s_active_slot, &repair);
    if (err == ESP_ERR_NOT_FOUND) {
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        s_snapshot.payload.schema_version =
            ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION;
        s_state = ARGUS_MACHINE_DIRECTORY_READY;
        err = commit_locked(&s_snapshot.payload, 0U);
    } else if (err == ESP_OK) {
        s_state = ARGUS_MACHINE_DIRECTORY_READY;
        bool a_valid = a_status == ESP_OK &&
                       argus_machine_directory_slot_valid(a);
        bool b_valid = b_status == ESP_OK &&
                       argus_machine_directory_slot_valid(b);
        s_redundancy_degraded = !(a_valid && b_valid);
        if (repair) {
            err = write_selector(s_active_slot);
        }
    } else {
        s_state = err == ESP_ERR_NOT_SUPPORTED
                      ? ARGUS_MACHINE_DIRECTORY_UNSUPPORTED
                      : ARGUS_MACHINE_DIRECTORY_CORRUPT;
    }
    argus_password_zeroize(a, sizeof(*a));
    argus_password_zeroize(b, sizeof(*b));
    free(a);
    free(b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Machine directory failed closed: %s",
                 esp_err_to_name(err));
        return err;
    }
    s_queue = xQueueCreate(MACHINE_QUEUE_LENGTH,
                           sizeof(machine_request_t *));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(writer_task, "argus_machine_dir",
                    MACHINE_WRITER_STACK, NULL,
                    MACHINE_WRITER_PRIORITY, &s_writer) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Machine directory ready (generation=%lu machines=%u)",
             (unsigned long)s_snapshot.generation,
             (unsigned)s_snapshot.payload.machine_count);
    return ESP_OK;
}

esp_err_t argus_machine_directory_get_snapshot(
    argus_machine_directory_snapshot_t *out)
{
    if (!s_initialized || out == NULL ||
        s_state != ARGUS_MACHINE_DIRECTORY_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t argus_machine_directory_get_status(
    argus_machine_directory_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->schema_version = s_snapshot.payload.schema_version;
    out->machine_count = s_snapshot.payload.machine_count;
    out->generation = s_snapshot.generation;
    out->redundancy_degraded = s_redundancy_degraded;
    xSemaphoreGive(s_mutex);
    return s_state == ARGUS_MACHINE_DIRECTORY_READY
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}

esp_err_t argus_machine_directory_commit(
    const argus_machine_directory_payload_t *payload,
    uint32_t expected_generation)
{
    if (!s_initialized || payload == NULL || s_queue == NULL ||
        s_state != ARGUS_MACHINE_DIRECTORY_READY ||
        xTaskGetCurrentTaskHandle() == s_writer) {
        return ESP_ERR_INVALID_STATE;
    }
    machine_request_t request = {
        .payload = malloc(sizeof(*payload)),
        .expected_generation = expected_generation,
        .result = ESP_FAIL,
    };
    if (request.payload == NULL) return ESP_ERR_NO_MEM;
    *request.payload = *payload;
    request.completion =
        xSemaphoreCreateBinaryStatic(&request.completion_storage);
    machine_request_t *request_ptr = &request;
    if (request.completion == NULL ||
        xQueueSend(s_queue, &request_ptr, pdMS_TO_TICKS(1000)) != pdTRUE) {
        argus_password_zeroize(request.payload, sizeof(*request.payload));
        free(request.payload);
        return ESP_ERR_TIMEOUT;
    }
    (void)xSemaphoreTake(request.completion, portMAX_DELAY);
    argus_password_zeroize(request.payload, sizeof(*request.payload));
    free(request.payload);
    return request.result;
}

esp_err_t argus_machine_directory_find(
    const char *identifier, argus_security_machine_record_t *out,
    size_t *out_index, uint32_t *out_directory_generation)
{
    if (!s_initialized || identifier == NULL || out == NULL ||
        s_state != ARGUS_MACHINE_DIRECTORY_READY) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0U; i < s_snapshot.payload.machine_count; ++i) {
        if (strcmp(identifier,
                   s_snapshot.payload.machines[i].identifier) == 0) {
            *out = s_snapshot.payload.machines[i];
            if (out_index != NULL) *out_index = i;
            if (out_directory_generation != NULL) {
                *out_directory_generation = s_snapshot.generation;
            }
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}
