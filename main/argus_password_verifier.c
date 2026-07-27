#include "argus_password_verifier.h"

#include <inttypes.h>
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
#include "mbedtls/platform_util.h"

#define ARGUS_KDF_QUEUE_LENGTH 1U
#define ARGUS_KDF_TASK_STACK 6144U
#define ARGUS_KDF_TASK_PRIORITY 3U
#define ARGUS_KDF_COOPERATE_INTERVAL 256U
// How long a caller waits for its turn on the serialized KDF worker before
// failing closed. Worst case is one in-progress derivation: the queue is
// depth 1, so a slot frees when the worker finishes the current item and
// dequeues the waiting one.
//
// MEASURED ON TARGET (ESP32-S3 @160MHz, [k] benchmark, 2026-07-26):
//     10,000 iterations ->   827 ms
//     25,000 iterations ->  2068 ms
//     50,000 iterations ->  4130 ms
//    100,000 iterations ->  8266 ms
// Linear at ~0.0827 ms/iteration.
//
// Every credential this system creates uses ARGUS_PASSWORD_ITERATIONS_DEFAULT
// (25,000) - machine enrollment, human login, provisioning and migration all
// pass DEFAULT - so the realistic worst case is 2068 ms. 8000 ms is ~3.9x
// that, which leaves room for the CPU contention that occurs when a station
// is associating. Retained on the strength of the measurement rather than
// changed for its own sake.
//
// KNOWN INCONSISTENCY, not fixed here: ARGUS_PASSWORD_ITERATIONS_MAX is
// 500,000, which extrapolates to ~41 s per derivation - five times this
// budget. Records carry their own iteration count and are verified at it, so
// a record stored above ~96,000 iterations would make concurrent
// authentication fail with exactly the timeout this wait exists to prevent.
// Nothing creates such a record today. Lowering ITERATIONS_MAX would risk
// invalidating an existing stored record, so it is reported rather than
// changed unilaterally. Either bound MAX to what this budget covers, or
// raise the budget, before any high-iteration record is ever issued.
#define ARGUS_KDF_SUBMIT_WAIT_MS 8000U

// Above this, one derivation exceeds ARGUS_KDF_SUBMIT_WAIT_MS and a
// concurrent authentication would be refused. Derived from the measurement
// above with a 20% safety margin.
#define ARGUS_KDF_ITERATIONS_WITHIN_BUDGET 80000U

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

static void cooperative_delay(void *ctx)
{
    (void)ctx;
    vTaskDelay(1U);
}

esp_err_t argus_password_pbkdf2_cooperative_for_test(
    const uint8_t *password, size_t password_len,
    const uint8_t *salt, size_t salt_len,
    uint32_t iterations, uint8_t out[ARGUS_PASSWORD_VERIFIER_SIZE],
    argus_password_cooperate_fn cooperate, void *cooperate_ctx)
{
    if (password == NULL || password_len == 0U || salt == NULL ||
        salt_len == 0U || iterations == 0U || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const mbedtls_md_info_t *md_info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) return ESP_FAIL;

    mbedtls_md_context_t md;
    uint8_t digest[ARGUS_PASSWORD_VERIFIER_SIZE] = {0};
    uint8_t accumulator[ARGUS_PASSWORD_VERIFIER_SIZE] = {0};
    static const uint8_t block_index[4] = {0U, 0U, 0U, 1U};
    mbedtls_md_init(&md);
    int result = mbedtls_md_setup(&md, md_info, 1);
    if (result == 0) result = mbedtls_md_hmac_starts(
        &md, password, password_len);
    if (result == 0) result = mbedtls_md_hmac_update(&md, salt, salt_len);
    if (result == 0) result = mbedtls_md_hmac_update(
        &md, block_index, sizeof(block_index));
    if (result == 0) result = mbedtls_md_hmac_finish(&md, digest);
    if (result == 0) result = mbedtls_md_hmac_reset(&md);
    if (result == 0) memcpy(accumulator, digest, sizeof(accumulator));

    for (uint32_t round = 1U; result == 0 && round < iterations; ++round) {
        result = mbedtls_md_hmac_update(&md, digest, sizeof(digest));
        if (result == 0) result = mbedtls_md_hmac_finish(&md, digest);
        if (result == 0) result = mbedtls_md_hmac_reset(&md);
        if (result != 0) break;
        for (size_t i = 0U; i < sizeof(accumulator); ++i) {
            accumulator[i] ^= digest[i];
        }
        if (cooperate != NULL &&
            (round % ARGUS_KDF_COOPERATE_INTERVAL) == 0U) {
            cooperate(cooperate_ctx);
        }
    }

    if (result == 0) memcpy(out, accumulator, sizeof(accumulator));
    mbedtls_platform_zeroize(digest, sizeof(digest));
    mbedtls_platform_zeroize(accumulator, sizeof(accumulator));
    mbedtls_md_free(&md);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t argus_password_pbkdf2_for_test(
    const uint8_t *password, size_t password_len,
    const uint8_t *salt, size_t salt_len,
    uint32_t iterations, uint8_t out[ARGUS_PASSWORD_VERIFIER_SIZE])
{
    return argus_password_pbkdf2_cooperative_for_test(
        password, password_len, salt, salt_len, iterations, out,
        cooperative_delay, NULL);
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
    // Surfaces the known inconsistency at the point it would actually bite:
    // a record whose iteration count takes longer to derive than another
    // caller is willing to wait for a queue slot. Warn rather than refuse -
    // the record is valid and this authentication will succeed; it is
    // CONCURRENT authentications that would be refused.
    if (record->iterations > ARGUS_KDF_ITERATIONS_WITHIN_BUDGET) {
        ESP_LOGW(TAG,
                 "record uses %" PRIu32 " iterations; one derivation exceeds the "
                 "%u ms concurrent-auth budget",
                 record->iterations, (unsigned)ARGUS_KDF_SUBMIT_WAIT_MS);
    }
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

    // The KDF worker is deliberately serialized - one derivation at a time,
    // so PBKDF2 cannot monopolise the CPU from several tasks at once. The
    // queue is therefore depth 1, and a caller arriving while another
    // derivation is queued must WAIT for its turn.
    //
    // This previously sent with zero wait and returned ESP_ERR_TIMEOUT
    // immediately. That turned "someone else is authenticating right now"
    // into "your credential is rejected": any two overlapping
    // authentications, and one of them failed. Reproduced repeatedly on
    // hardware - the HMI associating and authenticating mid-run made
    // test_4d2_verifier_create_verify fail four runs out of four. Every
    // caller here is an authentication path (argus_auth_service,
    // argus_machine_service, security migration and provisioning), so the
    // symptom in the field is an intermittently refused login or a refused
    // MQTT CONNECT, with nothing wrong with the credential. Raising the AP
    // client limit to 4 makes overlapping authentication more likely, not
    // less.
    //
    // Bounded rather than portMAX_DELAY: a caller must never be parked
    // forever behind a wedged worker. On expiry it still fails closed.
    if (xQueueSend(s_queue, &request, 0U) != pdTRUE) {
        ESP_LOGW(TAG,
                 "KDF worker busy (concurrent authentication) - waiting up to %u ms",
                 (unsigned)ARGUS_KDF_SUBMIT_WAIT_MS);
        if (xQueueSend(s_queue, &request,
                       pdMS_TO_TICKS(ARGUS_KDF_SUBMIT_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "KDF submit timed out after %u ms - failing closed",
                     (unsigned)ARGUS_KDF_SUBMIT_WAIT_MS);
            return ESP_ERR_TIMEOUT;
        }
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
