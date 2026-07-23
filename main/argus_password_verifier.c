#include "argus_password_verifier.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

#define ARGUS_KDF_QUEUE_LENGTH 4U
#define ARGUS_KDF_TASK_STACK 6144U
#define ARGUS_KDF_TASK_PRIORITY 3U

typedef enum {
    ARGUS_KDF_CREATE = 0,
    ARGUS_KDF_VERIFY,
    ARGUS_KDF_BENCHMARK,
} argus_kdf_operation_t;

typedef struct {
    argus_kdf_operation_t operation;
    const uint8_t *password;
    size_t password_len;
    uint32_t iterations;
    argus_password_verifier_t *record;
    bool *out_match;
    argus_password_benchmark_t *benchmark;
    SemaphoreHandle_t completion;
    StaticSemaphore_t completion_storage;
    esp_err_t result;
} argus_kdf_request_t;

static const char *TAG = "argus_password";
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;

void argus_password_zeroize(void *data, size_t length)
{
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (p != NULL && length-- > 0U) {
        *p++ = 0U;
    }
}

static bool input_valid(const uint8_t *password, size_t password_len)
{
    return password != NULL && password_len > 0U &&
           password_len <= ARGUS_PASSWORD_INPUT_MAX;
}

bool argus_password_verifier_record_valid(
    const argus_password_verifier_t *record)
{
    return record != NULL &&
           record->format_version == ARGUS_PASSWORD_FORMAT_VERSION &&
           record->algorithm ==
               ARGUS_PASSWORD_ALGORITHM_PBKDF2_HMAC_SHA256 &&
           record->salt_length == ARGUS_PASSWORD_SALT_SIZE &&
           record->verifier_length == ARGUS_PASSWORD_VERIFIER_SIZE &&
           record->iterations >= ARGUS_PASSWORD_ITERATIONS_MIN &&
           record->iterations <= ARGUS_PASSWORD_ITERATIONS_MAX;
}

esp_err_t argus_password_pbkdf2_for_test(
    const uint8_t *password, size_t password_len,
    const uint8_t *salt, size_t salt_len,
    uint32_t iterations, uint8_t out[ARGUS_PASSWORD_VERIFIER_SIZE])
{
    if (password == NULL || password_len == 0U || salt == NULL ||
        salt_len == 0U || iterations == 0U || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int result = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256, password, password_len, salt, salt_len,
        iterations, ARGUS_PASSWORD_VERIFIER_SIZE, out);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t derive_record(const uint8_t *password, size_t password_len,
                               uint32_t iterations,
                               argus_password_verifier_t *record)
{
    if (!input_valid(password, password_len) || record == NULL ||
        iterations < ARGUS_PASSWORD_ITERATIONS_MIN ||
        iterations > ARGUS_PASSWORD_ITERATIONS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(record, 0, sizeof(*record));
    record->format_version = ARGUS_PASSWORD_FORMAT_VERSION;
    record->algorithm = ARGUS_PASSWORD_ALGORITHM_PBKDF2_HMAC_SHA256;
    record->salt_length = ARGUS_PASSWORD_SALT_SIZE;
    record->verifier_length = ARGUS_PASSWORD_VERIFIER_SIZE;
    record->iterations = iterations;
    esp_fill_random(record->salt, sizeof(record->salt));

    esp_err_t result = argus_password_pbkdf2_for_test(
        password, password_len, record->salt, sizeof(record->salt),
        iterations, record->verifier);
    if (result != ESP_OK) {
        argus_password_zeroize(record, sizeof(*record));
    }
    return result;
}

static esp_err_t verify_record(const uint8_t *password, size_t password_len,
                               const argus_password_verifier_t *record,
                               bool *out_match)
{
    if (!input_valid(password, password_len) ||
        !argus_password_verifier_record_valid(record) || out_match == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_match = false;
    uint8_t candidate[ARGUS_PASSWORD_VERIFIER_SIZE] = {0};
    esp_err_t result = argus_password_pbkdf2_for_test(
        password, password_len, record->salt, record->salt_length,
        record->iterations, candidate);
    if (result == ESP_OK) {
        *out_match = mbedtls_ct_memcmp(candidate, record->verifier,
                                       sizeof(candidate)) == 0;
    }
    argus_password_zeroize(candidate, sizeof(candidate));
    return result;
}

static esp_err_t run_benchmark(uint32_t iterations,
                               argus_password_benchmark_t *out)
{
    if (iterations < ARGUS_PASSWORD_ITERATIONS_MIN ||
        iterations > ARGUS_PASSWORD_ITERATIONS_MAX || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const uint8_t synthetic_password[] =
        "phase4d2-synthetic-benchmark-only";
    static const uint8_t synthetic_salt[ARGUS_PASSWORD_SALT_SIZE] = {
        0x70, 0x34, 0x64, 0x32, 0x2d, 0x62, 0x65, 0x6e,
        0x63, 0x68, 0x2d, 0x73, 0x61, 0x6c, 0x74, 0x21,
    };
    uint8_t result[ARGUS_PASSWORD_VERIFIER_SIZE] = {0};
    memset(out, 0, sizeof(*out));
    out->iterations = iterations;
    out->free_heap_before = esp_get_free_heap_size();
    int64_t started = esp_timer_get_time();
    esp_err_t err = argus_password_pbkdf2_for_test(
        synthetic_password, sizeof(synthetic_password) - 1U,
        synthetic_salt, sizeof(synthetic_salt), iterations, result);
    int64_t elapsed = esp_timer_get_time() - started;
    out->free_heap_after = esp_get_free_heap_size();
    out->elapsed_ms = elapsed > 0 ? (uint32_t)(elapsed / 1000) : 0U;
    out->worker_stack_high_water_bytes =
        (uint32_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    argus_password_zeroize(result, sizeof(result));
    return err;
}

static void kdf_worker(void *ctx)
{
    (void)ctx;
    argus_kdf_request_t *request = NULL;
    for (;;) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE ||
            request == NULL) {
            continue;
        }
        switch (request->operation) {
            case ARGUS_KDF_CREATE:
                request->result = derive_record(
                    request->password, request->password_len,
                    request->iterations, request->record);
                break;
            case ARGUS_KDF_VERIFY:
                request->result = verify_record(
                    request->password, request->password_len,
                    request->record, request->out_match);
                break;
            case ARGUS_KDF_BENCHMARK:
                request->result = run_benchmark(
                    request->iterations, request->benchmark);
                break;
            default:
                request->result = ESP_ERR_INVALID_ARG;
                break;
        }
        xSemaphoreGive(request->completion);
    }
}

esp_err_t argus_password_verifier_init(void)
{
    if (s_worker != NULL) return ESP_OK;
    s_queue = xQueueCreate(ARGUS_KDF_QUEUE_LENGTH,
                           sizeof(argus_kdf_request_t *));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(kdf_worker, "argus_kdf", ARGUS_KDF_TASK_STACK, NULL,
                    ARGUS_KDF_TASK_PRIORITY, &s_worker) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "PBKDF2 worker initialized (stack=%u, queue=%u)",
             ARGUS_KDF_TASK_STACK, ARGUS_KDF_QUEUE_LENGTH);
    return ESP_OK;
}

static esp_err_t submit(argus_kdf_request_t *request)
{
    if (request == NULL || s_queue == NULL || s_worker == NULL ||
        xTaskGetCurrentTaskHandle() == s_worker) {
        return ESP_ERR_INVALID_STATE;
    }
    request->completion =
        xSemaphoreCreateBinaryStatic(&request->completion_storage);
    if (request->completion == NULL) return ESP_ERR_NO_MEM;
    request->result = ESP_FAIL;
    if (xQueueSend(s_queue, &request, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    (void)xSemaphoreTake(request->completion, portMAX_DELAY);
    return request->result;
}

esp_err_t argus_password_verifier_create(
    const uint8_t *password, size_t password_len, uint32_t iterations,
    argus_password_verifier_t *out_record)
{
    argus_kdf_request_t request = {
        .operation = ARGUS_KDF_CREATE,
        .password = password,
        .password_len = password_len,
        .iterations = iterations,
        .record = out_record,
    };
    return submit(&request);
}

esp_err_t argus_password_verifier_verify(
    const uint8_t *password, size_t password_len,
    const argus_password_verifier_t *record, bool *out_match)
{
    argus_kdf_request_t request = {
        .operation = ARGUS_KDF_VERIFY,
        .password = password,
        .password_len = password_len,
        .record = (argus_password_verifier_t *)record,
        .out_match = out_match,
    };
    return submit(&request);
}

esp_err_t argus_password_verifier_benchmark(
    uint32_t iterations, argus_password_benchmark_t *out_result)
{
    argus_kdf_request_t request = {
        .operation = ARGUS_KDF_BENCHMARK,
        .iterations = iterations,
        .benchmark = out_result,
    };
    return submit(&request);
}
