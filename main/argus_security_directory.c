#include "argus_security_directory.h"

#include <stdlib.h>
#include <string.h>

#include "argus_password_verifier.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define DIRECTORY_NAMESPACE "argus_dir"
#define DIRECTORY_SLOT_A "directory_a"
#define DIRECTORY_SLOT_B "directory_b"
#define DIRECTORY_SELECTOR "directory_sel"
#define DIRECTORY_MAGIC UINT32_C(0x41524433)
#define DIRECTORY_VALID UINT32_C(0x56414c44)
#define DIRECTORY_QUEUE_LENGTH 2U
#define DIRECTORY_WRITER_STACK 6144U
#define DIRECTORY_WRITER_PRIORITY 3U

typedef struct {
    argus_security_directory_payload_t *payload;
    uint32_t expected_generation;
    SemaphoreHandle_t completion;
    StaticSemaphore_t completion_storage;
    esp_err_t result;
} directory_request_t;

static const char *TAG = "argus_sec_dir";
static argus_security_directory_snapshot_t s_snapshot;
static uint8_t s_active_slot;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;
static QueueHandle_t s_queue;
static TaskHandle_t s_writer;
static bool s_initialized;

_Static_assert(sizeof(argus_security_directory_payload_t) < 8192U,
               "Security directory exceeds bounded storage design");

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

uint32_t argus_security_directory_crc32(
    const argus_security_directory_payload_t *payload)
{
    return payload == NULL
               ? 0U
               : crc32_bytes((const uint8_t *)payload, sizeof(*payload));
}

bool argus_security_directory_payload_valid(
    const argus_security_directory_payload_t *payload)
{
    if (payload == NULL ||
        payload->schema_version != ARGUS_SECURITY_DIRECTORY_SCHEMA_VERSION ||
        payload->flags != 0U || payload->reserved != 0U ||
        payload->custom_role_count > ARGUS_SECURITY_CUSTOM_ROLE_CAPACITY ||
        payload->human_count > ARGUS_SECURITY_MAX_HUMANS) {
        return false;
    }
    for (size_t i = 0U; i < ARGUS_SECURITY_CUSTOM_ROLE_CAPACITY; ++i) {
        if (i < payload->custom_role_count) {
            const argus_security_role_record_t *role =
                &payload->custom_roles[i];
            if (!argus_security_role_record_valid(role) ||
                role->builtin != 0U ||
                role->level == ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL ||
                (role->permissions &
                 ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) != 0U) {
                return false;
            }
            for (size_t j = 0U; j < i; ++j) {
                if (strcmp(role->identifier,
                           payload->custom_roles[j].identifier) == 0) {
                    return false;
                }
            }
        } else {
            static const argus_security_role_record_t zero = {0};
            if (memcmp(&payload->custom_roles[i], &zero, sizeof(zero)) != 0) {
                return false;
            }
        }
    }
    for (size_t i = 0U; i < ARGUS_SECURITY_MAX_HUMANS; ++i) {
        if (i < payload->human_count) {
            const argus_security_human_record_t *human =
                &payload->humans[i];
            if (!argus_security_human_record_valid(human)) return false;
            size_t role_count = ARGUS_SECURITY_BUILTIN_ROLE_COUNT +
                                payload->custom_role_count;
            uint16_t allowed_roles = role_count >= 16U
                ? UINT16_MAX
                : (uint16_t)((UINT16_C(1) << role_count) - 1U);
            uint16_t required_builtin =
                (uint16_t)(UINT16_C(1) << human->level);
            if ((human->role_mask & ~allowed_roles) != 0U ||
                (human->role_mask & required_builtin) == 0U) {
                return false;
            }
            for (size_t j = 0U; j < i; ++j) {
                if (strcmp(human->identifier,
                           payload->humans[j].identifier) == 0 ||
                    strcmp(human->login, payload->humans[j].login) == 0) {
                    return false;
                }
            }
        } else {
            static const argus_security_human_record_t zero = {0};
            if (memcmp(&payload->humans[i], &zero, sizeof(zero)) != 0) {
                return false;
            }
        }
    }
    return true;
}

static bool slot_valid(const argus_security_directory_slot_t *slot)
{
    return slot != NULL && slot->magic == DIRECTORY_MAGIC &&
           slot->schema_version == ARGUS_SECURITY_DIRECTORY_SCHEMA_VERSION &&
           slot->payload_length == sizeof(slot->payload) &&
           slot->generation != 0U && slot->valid_marker == DIRECTORY_VALID &&
           slot->crc32 == argus_security_directory_crc32(&slot->payload) &&
           argus_security_directory_payload_valid(&slot->payload);
}

static esp_err_t open_directory(nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open_from_partition(
        ARGUS_SECURITY_PARTITION, DIRECTORY_NAMESPACE, mode, out);
}

static esp_err_t read_slot(uint8_t index,
                           argus_security_directory_slot_t *out)
{
    nvs_handle_t handle;
    esp_err_t err = open_directory(NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t length = sizeof(*out);
    err = nvs_get_blob(handle, index == 0U ? DIRECTORY_SLOT_A :
                                             DIRECTORY_SLOT_B,
                       out, &length);
    nvs_close(handle);
    return err;
}

static esp_err_t write_slot(uint8_t index,
                            const argus_security_directory_slot_t *slot)
{
    nvs_handle_t handle;
    esp_err_t err = open_directory(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, index == 0U ? DIRECTORY_SLOT_A :
                                            DIRECTORY_SLOT_B,
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
    err = nvs_get_u8(handle, DIRECTORY_SELECTOR, out);
    nvs_close(handle);
    return err;
}

static esp_err_t write_selector(uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = open_directory(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, DIRECTORY_SELECTOR, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t commit_locked(
    const argus_security_directory_payload_t *payload,
    uint32_t expected_generation)
{
    if (!argus_security_directory_payload_valid(payload) ||
        expected_generation != s_snapshot.generation) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t target = s_snapshot.generation == 0U
                         ? 0U
                         : (uint8_t)(s_active_slot ^ 1U);
    uint32_t generation = s_snapshot.generation + 1U;
    if (generation == 0U) generation = 1U;
    argus_security_directory_slot_t *slot =
        calloc(1U, sizeof(*slot));
    argus_security_directory_slot_t *readback =
        calloc(1U, sizeof(*readback));
    if (slot == NULL || readback == NULL) {
        free(slot);
        free(readback);
        return ESP_ERR_NO_MEM;
    }
    *slot = (argus_security_directory_slot_t) {
        .magic = DIRECTORY_MAGIC,
        .schema_version = ARGUS_SECURITY_DIRECTORY_SCHEMA_VERSION,
        .payload_length = sizeof(*payload),
        .generation = generation,
        .crc32 = argus_security_directory_crc32(payload),
        .valid_marker = DIRECTORY_VALID,
        .payload = *payload,
    };
    esp_err_t err = write_slot(target, slot);
    if (err == ESP_OK) err = read_slot(target, readback);
    if (err == ESP_OK &&
        (!slot_valid(readback) ||
         memcmp(slot, readback, sizeof(*slot)) != 0)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) err = write_selector(target);
    if (err == ESP_OK) {
        s_snapshot.payload = *payload;
        s_snapshot.generation = generation;
        s_active_slot = target;
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
    directory_request_t *request = NULL;
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

esp_err_t argus_security_directory_init(void)
{
    if (s_initialized) return ESP_OK;
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    argus_security_directory_slot_t *a = calloc(1U, sizeof(*a));
    argus_security_directory_slot_t *b = calloc(1U, sizeof(*b));
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t a_err = read_slot(0U, a);
    esp_err_t b_err = read_slot(1U, b);
    bool a_missing = a_err == ESP_ERR_NVS_NOT_FOUND ||
                     a_err == ESP_ERR_NOT_FOUND;
    bool b_missing = b_err == ESP_ERR_NVS_NOT_FOUND ||
                     b_err == ESP_ERR_NOT_FOUND;
    bool a_valid = a_err == ESP_OK && slot_valid(a);
    bool b_valid = b_err == ESP_OK && slot_valid(b);
    if ((!a_missing && a_err != ESP_OK) || (!b_missing && b_err != ESP_OK)) {
        free(a);
        free(b);
        return ESP_ERR_INVALID_STATE;
    }
    if (!a_valid && !b_valid && !(a_missing && b_missing)) {
        free(a);
        free(b);
        ESP_LOGE(TAG, "Security directory malformed; failing closed");
        return ESP_ERR_INVALID_STATE;
    }
    if (a_missing && b_missing) {
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        s_snapshot.payload.schema_version =
            ARGUS_SECURITY_DIRECTORY_SCHEMA_VERSION;
        esp_err_t err = commit_locked(&s_snapshot.payload, 0U);
        free(a);
        free(b);
        if (err != ESP_OK) return err;
    } else {
        uint8_t selector;
        esp_err_t selector_err = read_selector(&selector);
        if (selector_err != ESP_OK || selector > 1U ||
            (selector == 0U && !a_valid) ||
            (selector == 1U && !b_valid)) {
            free(a);
            free(b);
            ESP_LOGE(TAG, "Security directory selector invalid");
            return ESP_ERR_INVALID_STATE;
        }
        argus_security_directory_slot_t *selected =
            selector == 0U ? a : b;
        s_snapshot.payload = selected->payload;
        s_snapshot.generation = selected->generation;
        s_active_slot = selector;
        free(a);
        free(b);
    }
    s_queue = xQueueCreate(DIRECTORY_QUEUE_LENGTH,
                           sizeof(directory_request_t *));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(writer_task, "argus_sec_dir",
                    DIRECTORY_WRITER_STACK, NULL,
                    DIRECTORY_WRITER_PRIORITY, &s_writer) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Security directory ready (generation=%lu humans=%u roles=%u)",
             (unsigned long)s_snapshot.generation,
             (unsigned)s_snapshot.payload.human_count,
             (unsigned)s_snapshot.payload.custom_role_count);
    return ESP_OK;
}

esp_err_t argus_security_directory_get_snapshot(
    argus_security_directory_snapshot_t *out)
{
    if (!s_initialized || out == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t argus_security_directory_commit(
    const argus_security_directory_payload_t *payload,
    uint32_t expected_generation)
{
    if (!s_initialized || payload == NULL ||
        s_queue == NULL || xTaskGetCurrentTaskHandle() == s_writer) {
        return ESP_ERR_INVALID_STATE;
    }
    directory_request_t request = {
        .payload = malloc(sizeof(*payload)),
        .expected_generation = expected_generation,
        .result = ESP_FAIL,
    };
    if (request.payload == NULL) return ESP_ERR_NO_MEM;
    *request.payload = *payload;
    request.completion =
        xSemaphoreCreateBinaryStatic(&request.completion_storage);
    directory_request_t *request_ptr = &request;
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

esp_err_t argus_security_directory_find_login(
    const char *canonical_login,
    argus_security_human_record_t *out)
{
    if (!s_initialized || canonical_login == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0U; i < s_snapshot.payload.human_count; ++i) {
        if (strcmp(s_snapshot.payload.humans[i].login,
                   canonical_login) == 0) {
            *out = s_snapshot.payload.humans[i];
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t argus_security_directory_find_id(
    const char *identifier,
    argus_security_human_record_t *out,
    size_t *out_index)
{
    if (!s_initialized || identifier == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0U; i < s_snapshot.payload.human_count; ++i) {
        if (strcmp(s_snapshot.payload.humans[i].identifier,
                   identifier) == 0) {
            *out = s_snapshot.payload.humans[i];
            if (out_index != NULL) *out_index = i;
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}
