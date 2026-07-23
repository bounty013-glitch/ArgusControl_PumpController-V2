#include "argus_local_recovery.h"

#include <string.h>

#include "argus_net_mgr.h"
#include "argus_password_verifier.h"
#include "argus_security_store.h"
#include "argus_state_mgr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ARGUS_RECOVERY_TASK_STACK 3072U
#define ARGUS_RECOVERY_TASK_PRIORITY 2U

static const char *TAG = "argus_recovery";
static argus_recovery_detector_t s_detector;
static SemaphoreHandle_t s_status_mutex;
static StaticSemaphore_t s_status_mutex_storage;
static TaskHandle_t s_task;
static uint32_t s_trigger_count;

static uint32_t add_bounded(uint32_t value, uint32_t increment)
{
    return UINT32_MAX - value < increment ? UINT32_MAX : value + increment;
}

void argus_recovery_detector_init(argus_recovery_detector_t *detector,
                                  bool initial_high)
{
    if (detector == NULL) return;
    memset(detector, 0, sizeof(*detector));
    detector->raw_high = initial_high;
    detector->stable_high = initial_high;
}

argus_recovery_detector_result_t argus_recovery_detector_update(
    argus_recovery_detector_t *detector, bool level_high,
    uint32_t elapsed_ms)
{
    if (detector == NULL || elapsed_ms == 0U || detector->triggered) {
        return ARGUS_RECOVERY_DETECT_NONE;
    }
    if (level_high != detector->raw_high) {
        detector->raw_high = level_high;
        detector->raw_stable_ms = elapsed_ms;
    } else {
        detector->raw_stable_ms =
            add_bounded(detector->raw_stable_ms, elapsed_ms);
    }
    if (detector->raw_stable_ms < ARGUS_RECOVERY_DEBOUNCE_MS) {
        return ARGUS_RECOVERY_DETECT_NONE;
    }

    bool changed = !detector->stable_initialized ||
                   detector->stable_high != detector->raw_high;
    bool prior_high = detector->stable_high;
    detector->stable_high = detector->raw_high;
    detector->stable_initialized = true;

    if (detector->stable_high) {
        detector->startup_release_seen = true;
        detector->held_ms = 0U;
        if (changed && !prior_high && detector->hold_qualified) {
            detector->triggered = true;
            return ARGUS_RECOVERY_DETECT_QUALIFIED_RELEASE;
        }
        detector->hold_qualified = false;
        return ARGUS_RECOVERY_DETECT_NONE;
    }

    if (!detector->startup_release_seen) {
        detector->held_ms = 0U;
        detector->hold_qualified = false;
        return ARGUS_RECOVERY_DETECT_NONE;
    }
    if (changed) detector->held_ms = 0U;
    detector->held_ms = add_bounded(detector->held_ms, elapsed_ms);
    if (detector->held_ms >= ARGUS_RECOVERY_HOLD_MS) {
        detector->hold_qualified = true;
    }
    return ARGUS_RECOVERY_DETECT_NONE;
}

argus_local_recovery_commit_result_t argus_local_recovery_commit(
    const argus_local_recovery_ops_t *ops)
{
    argus_local_recovery_commit_result_t result = {.error = ESP_OK};
    if (ops == NULL || ops->persist_request == NULL ||
        ops->post_network_request == NULL) {
        result.error = ESP_ERR_INVALID_ARG;
        return result;
    }
    result.error = ops->persist_request(ops->ctx);
    if (result.error != ESP_OK) return result;
    result.persisted = true;
    result.error = ops->post_network_request(ops->ctx);
    if (result.error != ESP_OK) return result;
    result.network_posted = true;
    result.accepted = true;
    return result;
}

static esp_err_t persist_request(void *ctx)
{
    (void)ctx;
    return argus_security_store_set_recovery_state(
        ARGUS_SECURITY_RECOVERY_REQUESTED);
}

static esp_err_t post_network_request(void *ctx)
{
    (void)ctx;
    return argus_net_mgr_request_security_recovery();
}

static void recovery_task(void *ctx)
{
    (void)ctx;
    const argus_local_recovery_ops_t ops = {
        .persist_request = persist_request,
        .post_network_request = post_network_request,
    };
    for (;;) {
        bool high = gpio_get_level((gpio_num_t)ARGUS_RECOVERY_GPIO) != 0;
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        argus_recovery_detector_result_t detector_result =
            argus_recovery_detector_update(
                &s_detector, high, ARGUS_RECOVERY_SAMPLE_MS);
        xSemaphoreGive(s_status_mutex);
        if (detector_result == ARGUS_RECOVERY_DETECT_QUALIFIED_RELEASE) {
            argus_local_recovery_commit_result_t result =
                argus_local_recovery_commit(&ops);
            if (result.accepted) {
                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                s_trigger_count++;
                xSemaphoreGive(s_status_mutex);
                ESP_LOGW(TAG, "Physical local network recovery accepted");
            } else {
                ESP_LOGE(TAG,
                         "Physical local recovery failed after release: %s",
                         esp_err_to_name(result.error));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ARGUS_RECOVERY_SAMPLE_MS));
    }
}

esp_err_t argus_local_recovery_init(void)
{
    if (s_task != NULL) return ESP_OK;
    gpio_config_t config = {
        .pin_bit_mask = UINT64_C(1) << ARGUS_RECOVERY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    s_status_mutex = xSemaphoreCreateMutexStatic(&s_status_mutex_storage);
    if (s_status_mutex == NULL) return ESP_ERR_NO_MEM;
    argus_recovery_detector_init(
        &s_detector,
        gpio_get_level((gpio_num_t)ARGUS_RECOVERY_GPIO) != 0);
    if (xTaskCreate(recovery_task, "argus_recovery",
                    ARGUS_RECOVERY_TASK_STACK, NULL,
                    ARGUS_RECOVERY_TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "KEY1/GPIO0 detector armed after boot (release, debounce=%ums, hold=%ums)",
             ARGUS_RECOVERY_DEBOUNCE_MS, ARGUS_RECOVERY_HOLD_MS);
    return ESP_OK;
}

esp_err_t argus_local_recovery_get_status(
    argus_local_recovery_status_t *out_status)
{
    if (out_status == NULL || s_status_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out_status = (argus_local_recovery_status_t) {
        .initialized = s_task != NULL,
        .startup_release_seen = s_detector.startup_release_seen,
        .hold_qualified = s_detector.hold_qualified,
        .triggered = s_detector.triggered,
        .held_ms = s_detector.held_ms,
        .trigger_count = s_trigger_count,
    };
    xSemaphoreGive(s_status_mutex);
    return ESP_OK;
}

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
esp_err_t argus_local_recovery_clear_for_test(void)
{
    argus_state_snapshot_t state;
    argus_state_mgr_get_snapshot(&state);
    bool stationary =
        (state.machine_state == ARGUS_STATE_UNLOCKED ||
         state.machine_state == ARGUS_STATE_HOLDING) &&
        state.configured_target_rpm_milli == 0 &&
        state.trajectory_target_rpm_milli == 0 &&
        state.applied_rpm_milli == 0 && state.generated_rpm_milli == 0 &&
        !state.driver_enabled && !state.estop_latched &&
        state.fault_code == 0U;
    if (!stationary) return ESP_ERR_INVALID_STATE;
    esp_err_t err = argus_security_store_set_recovery_state(
        ARGUS_SECURITY_RECOVERY_INACTIVE);
    if (err != ESP_OK) return err;
    ESP_LOGW(TAG, "Diagnostic recovery cleanup accepted; restarting");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}
#endif
