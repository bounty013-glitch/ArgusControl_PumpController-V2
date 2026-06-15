#include "argus_stepper.h"

#include <limits.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "argus_stepper";

#define ARGUS_MIN_STEP_FREQ_HZ 1U
#define ARGUS_DEFAULT_MAX_STEP_FREQ_HZ 200000U
#define ARGUS_DEFAULT_FULL_STEPS_PER_REV 200U
#define ARGUS_DEFAULT_MICROSTEPS 32U
#define ARGUS_DEFAULT_GEARBOX_RATIO_NUM 10U
#define ARGUS_DEFAULT_GEARBOX_RATIO_DEN 1U
#define ARGUS_DEFAULT_RAMP_INTERVAL_MS 20U
#define ARGUS_DEFAULT_RAMP_RPM_PER_SEC_MILLI 10000U
#define ARGUS_PCNT_HIGH_LIMIT 32767

typedef struct {
    argus_stepper_hw_config_t hw;
    pcnt_unit_handle_t pcnt_unit;
    pcnt_channel_handle_t pcnt_channel;
    SemaphoreHandle_t lock;
    uint32_t high_word_count;
    int32_t commanded_speed_hz;
    int32_t applied_speed_hz;
    bool run_commanded;
    bool direction_forward;
    bool enabled;
    bool emergency_stopped;
    bool initialized;
} argus_stepper_context_t;

static argus_stepper_context_t s_ctx;

static bool IRAM_ATTR argus_pcnt_on_reach(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *user_ctx)
{
    (void)unit;
    argus_stepper_context_t *ctx = (argus_stepper_context_t *)user_ctx;
    if (edata->watch_point_value == ARGUS_PCNT_HIGH_LIMIT) {
        ctx->high_word_count += ARGUS_PCNT_HIGH_LIMIT;
        pcnt_unit_clear_count(ctx->pcnt_unit);
    }
    return false;
}

static inline bool argus_enable_level(bool enabled)
{
    return s_ctx.hw.enable_pin_active_high ? enabled : !enabled;
}

static uint32_t argus_motor_full_steps_per_rev(void)
{
    return (s_ctx.hw.motor_full_steps_per_rev > 0U) ? s_ctx.hw.motor_full_steps_per_rev : ARGUS_DEFAULT_FULL_STEPS_PER_REV;
}

static uint32_t argus_microsteps(void)
{
    return (s_ctx.hw.microsteps > 0U) ? s_ctx.hw.microsteps : ARGUS_DEFAULT_MICROSTEPS;
}

static uint32_t argus_gearbox_ratio_num(void)
{
    return (s_ctx.hw.gearbox_ratio_num > 0U) ? s_ctx.hw.gearbox_ratio_num : ARGUS_DEFAULT_GEARBOX_RATIO_NUM;
}

static uint32_t argus_gearbox_ratio_den(void)
{
    return (s_ctx.hw.gearbox_ratio_den > 0U) ? s_ctx.hw.gearbox_ratio_den : ARGUS_DEFAULT_GEARBOX_RATIO_DEN;
}

static uint64_t argus_output_steps_per_rev_u64(void)
{
    return (uint64_t)argus_motor_full_steps_per_rev() * (uint64_t)argus_microsteps() * (uint64_t)argus_gearbox_ratio_num() / (uint64_t)argus_gearbox_ratio_den();
}

static uint32_t argus_output_steps_per_rev(void)
{
    uint64_t value = argus_output_steps_per_rev_u64();
    if (value == 0U) {
        return ARGUS_DEFAULT_FULL_STEPS_PER_REV * ARGUS_DEFAULT_MICROSTEPS * ARGUS_DEFAULT_GEARBOX_RATIO_NUM;
    }
    if (value > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)value;
}

static uint32_t argus_max_motor_rpm(void)
{
    return (s_ctx.hw.max_motor_rpm > 0U) ? s_ctx.hw.max_motor_rpm : 2000U;
}

static uint32_t argus_max_output_rpm_milli(void)
{
    const uint64_t max_output_rpm_milli =
        ((uint64_t)argus_max_motor_rpm() * 1000ULL * (uint64_t)argus_gearbox_ratio_den()) / (uint64_t)argus_gearbox_ratio_num();

    if (max_output_rpm_milli > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)max_output_rpm_milli;
}

static uint32_t argus_max_step_freq_hz(void)
{
    return (s_ctx.hw.max_step_freq_hz > 0U) ? s_ctx.hw.max_step_freq_hz : ARGUS_DEFAULT_MAX_STEP_FREQ_HZ;
}

static uint32_t argus_ramp_interval_ms(void)
{
    return (s_ctx.hw.ramp_interval_ms > 0U) ? s_ctx.hw.ramp_interval_ms : ARGUS_DEFAULT_RAMP_INTERVAL_MS;
}

static uint32_t argus_ramp_rpm_per_sec_milli(void)
{
    return (s_ctx.hw.ramp_rpm_per_sec_milli > 0U) ? s_ctx.hw.ramp_rpm_per_sec_milli : ARGUS_DEFAULT_RAMP_RPM_PER_SEC_MILLI;
}

static uint32_t argus_abs_speed_hz(int32_t speed_hz)
{
    return (speed_hz < 0) ? (uint32_t)(-(int64_t)speed_hz) : (uint32_t)speed_hz;
}

static int32_t argus_steps_hz_to_output_rpm_milli(int32_t speed_hz)
{
    const int64_t steps_per_rev = (int64_t)argus_output_steps_per_rev();
    if (steps_per_rev <= 0) {
        return 0;
    }

    const int64_t rpm_milli = ((int64_t)speed_hz * 60000LL) / steps_per_rev;
    if (rpm_milli > INT32_MAX) {
        return INT32_MAX;
    }
    if (rpm_milli < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)rpm_milli;
}

static int32_t argus_output_rpm_milli_to_steps_hz(int32_t rpm_milli)
{
    const int64_t steps_per_rev = (int64_t)argus_output_steps_per_rev();
    const int64_t steps_hz = ((int64_t)rpm_milli * steps_per_rev) / 60000LL;

    if (steps_hz > INT32_MAX) {
        return INT32_MAX;
    }
    if (steps_hz < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)steps_hz;
}

static int32_t argus_position_steps_to_output_mrev(uint32_t position_steps)
{
    const int64_t steps_per_rev = (int64_t)argus_output_steps_per_rev();
    if (steps_per_rev <= 0) {
        return 0;
    }

    const int64_t mrev = ((int64_t)position_steps * 1000LL) / steps_per_rev;
    if (mrev > INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)mrev;
}

static int32_t argus_clamp_speed_hz(int32_t speed_hz)
{
    const uint32_t abs_speed_hz = argus_abs_speed_hz(speed_hz);
    const uint32_t max_speed_hz = argus_max_step_freq_hz();

    if (abs_speed_hz <= max_speed_hz) {
        return speed_hz;
    }

    return (speed_hz < 0) ? -(int32_t)max_speed_hz : (int32_t)max_speed_hz;
}

static uint32_t argus_ramp_step_hz(void)
{
    const uint64_t step_hz =
        ((uint64_t)argus_ramp_rpm_per_sec_milli() *
         (uint64_t)argus_ramp_interval_ms() *
         (uint64_t)argus_output_steps_per_rev()) /
        60000000ULL;

    if (step_hz == 0U) {
        return 1U;
    }
    if (step_hz > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)step_hz;
}

static int32_t argus_move_toward_speed(int32_t current_hz, int32_t target_hz, uint32_t step_hz)
{
    if (current_hz == target_hz) {
        return current_hz;
    }

    if (current_hz < target_hz) {
        const int64_t next_hz = (int64_t)current_hz + (int64_t)step_hz;
        return (next_hz > target_hz) ? target_hz : (int32_t)next_hz;
    }

    const int64_t next_hz = (int64_t)current_hz - (int64_t)step_hz;
    return (next_hz < target_hz) ? target_hz : (int32_t)next_hz;
}

static esp_err_t argus_step_output_set_freq(uint32_t freq_hz)
{
    if (freq_hz == 0U) {
        ESP_RETURN_ON_ERROR(ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0), TAG, "failed to stop ledc");
        return gpio_set_level(s_ctx.hw.step_gpio, 0);
    }

    if (freq_hz < ARGUS_MIN_STEP_FREQ_HZ) {
        freq_hz = ARGUS_MIN_STEP_FREQ_HZ;
    }
    if (freq_hz > argus_max_step_freq_hz()) {
        freq_hz = argus_max_step_freq_hz();
    }

    ESP_RETURN_ON_ERROR(ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz), TAG, "failed to set step frequency");
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512), TAG, "failed to set duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void argus_stepper_enable_driver(bool enabled)
{
    s_ctx.enabled = enabled;
    if (s_ctx.hw.has_enable_pin) {
        gpio_set_level(s_ctx.hw.en_gpio, argus_enable_level(enabled));
    }
}

static esp_err_t argus_pcnt_init(gpio_num_t step_gpio)
{
    pcnt_unit_config_t unit_config = {
        .high_limit = ARGUS_PCNT_HIGH_LIMIT,
        .low_limit = -32767,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_config, &s_ctx.pcnt_unit), TAG, "failed to create pcnt unit");

    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = step_gpio,
        .level_gpio_num = -1,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_ctx.pcnt_unit, &chan_config, &s_ctx.pcnt_channel), TAG, "failed to create pcnt channel");
    ESP_RETURN_ON_ERROR(
        pcnt_channel_set_edge_action(s_ctx.pcnt_channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD),
        TAG,
        "failed to configure pcnt edge action");
    ESP_RETURN_ON_ERROR(
        pcnt_channel_set_level_action(s_ctx.pcnt_channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP),
        TAG,
        "failed to configure pcnt level action");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(s_ctx.pcnt_unit, ARGUS_PCNT_HIGH_LIMIT), TAG, "failed to add watch point");

    pcnt_event_callbacks_t callbacks = {
        .on_reach = argus_pcnt_on_reach,
    };
    ESP_RETURN_ON_ERROR(pcnt_unit_register_event_callbacks(s_ctx.pcnt_unit, &callbacks, &s_ctx), TAG, "failed to register pcnt callbacks");
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_ctx.pcnt_unit), TAG, "failed to enable pcnt");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_ctx.pcnt_unit), TAG, "failed to clear pcnt");
    return pcnt_unit_start(s_ctx.pcnt_unit);
}

static esp_err_t argus_step_output_init(gpio_num_t step_gpio, uint32_t initial_freq_hz)
{
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = initial_freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "failed to configure ledc timer");

    ledc_channel_config_t channel_config = {
        .gpio_num = step_gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 512,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG, "failed to configure ledc channel");
    return argus_step_output_set_freq(0);
}

static uint32_t argus_get_position_steps_locked(void)
{
    int pcnt_count = 0;
    pcnt_unit_get_count(s_ctx.pcnt_unit, &pcnt_count);
    return s_ctx.high_word_count + (uint32_t)pcnt_count;
}

static esp_err_t argus_set_applied_output_locked(int32_t speed_hz)
{
    speed_hz = argus_clamp_speed_hz(speed_hz);

    const uint32_t abs_speed_hz = argus_abs_speed_hz(speed_hz);
    if (abs_speed_hz == 0U) {
        s_ctx.applied_speed_hz = 0;
        return argus_step_output_set_freq(0);
    }

    s_ctx.direction_forward = (speed_hz >= 0);
    gpio_set_level(s_ctx.hw.dir_gpio, s_ctx.direction_forward ? 1 : 0);

    s_ctx.applied_speed_hz = speed_hz;
    return argus_step_output_set_freq(abs_speed_hz);
}

static int32_t argus_target_speed_locked(void)
{
    if (!s_ctx.run_commanded) {
        return 0;
    }
    return argus_clamp_speed_hz(s_ctx.commanded_speed_hz);
}

static void argus_ramp_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(argus_ramp_interval_ms()));

        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);

        esp_err_t err = ESP_OK;
        const int32_t target_speed_hz = argus_target_speed_locked();
        if (s_ctx.applied_speed_hz != target_speed_hz) {
            const int32_t next_speed_hz =
                argus_move_toward_speed(s_ctx.applied_speed_hz, target_speed_hz, argus_ramp_step_hz());
            err = argus_set_applied_output_locked(next_speed_hz);
        }

        xSemaphoreGive(s_ctx.lock);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ramp update failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t argus_stepper_init(const argus_stepper_hw_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.hw = *config;
    s_ctx.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create mutex");

    gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << s_ctx.hw.dir_gpio) | (s_ctx.hw.has_enable_pin ? (1ULL << s_ctx.hw.en_gpio) : 0ULL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_config), TAG, "failed to configure outputs");

    ESP_RETURN_ON_ERROR(argus_step_output_init(s_ctx.hw.step_gpio, s_ctx.hw.ledc_freq_hz), TAG, "failed to init step output");
    ESP_RETURN_ON_ERROR(argus_pcnt_init(s_ctx.hw.step_gpio), TAG, "failed to init pcnt");

    gpio_set_level(s_ctx.hw.dir_gpio, 1);
    argus_stepper_enable_driver(false);

    s_ctx.initialized = true;
    BaseType_t created = xTaskCreate(argus_ramp_task, "argus_ramp", 3072, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create ramp task");

    ESP_LOGI(TAG,
             "stepper init done: step=%d dir=%d en=%d max_freq=%lu output_steps_per_rev=%lu ramp=%lu.%03lu rpm/s",
             s_ctx.hw.step_gpio,
             s_ctx.hw.dir_gpio,
             s_ctx.hw.en_gpio,
             (unsigned long)argus_max_step_freq_hz(),
             (unsigned long)argus_output_steps_per_rev(),
             (unsigned long)(argus_ramp_rpm_per_sec_milli() / 1000U),
             (unsigned long)(argus_ramp_rpm_per_sec_milli() % 1000U));
    return ESP_OK;
}

esp_err_t argus_stepper_set_speed(int32_t speed_hz)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "stepper not initialized");

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.commanded_speed_hz = argus_clamp_speed_hz(speed_hz);
    int32_t commanded_speed_hz = s_ctx.commanded_speed_hz;
    xSemaphoreGive(s_ctx.lock);

    ESP_LOGI(TAG, "speed target set: command=%ld", (long)commanded_speed_hz);
    return ESP_OK;
}

esp_err_t argus_stepper_set_speed_rpm_milli(int32_t rpm_milli)
{
    const uint32_t max_output_rpm_milli = argus_max_output_rpm_milli();
    int32_t clamped_rpm_milli = rpm_milli;

    if (rpm_milli > 0 && (uint32_t)rpm_milli > max_output_rpm_milli) {
        clamped_rpm_milli = (int32_t)max_output_rpm_milli;
    } else if (rpm_milli < 0 && (uint32_t)(-rpm_milli) > max_output_rpm_milli) {
        clamped_rpm_milli = -(int32_t)max_output_rpm_milli;
    }

    const int32_t speed_hz = argus_output_rpm_milli_to_steps_hz(clamped_rpm_milli);
    esp_err_t err = argus_stepper_set_speed(speed_hz);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "speed set from rpm: command=%ld.%03ld rpm",
                 (long)(clamped_rpm_milli / 1000),
                 (long)((clamped_rpm_milli < 0 ? -clamped_rpm_milli : clamped_rpm_milli) % 1000));
    }
    return err;
}

esp_err_t argus_stepper_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "stepper not initialized");

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);

    if (!s_ctx.enabled) {
        argus_stepper_enable_driver(true);
        xSemaphoreGive(s_ctx.lock);
        if (s_ctx.hw.enable_delay_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(s_ctx.hw.enable_delay_ms));
        }
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    }

    s_ctx.run_commanded = true;
    s_ctx.emergency_stopped = false;
    xSemaphoreGive(s_ctx.lock);

    ESP_LOGI(TAG, "stepper started");
    return ESP_OK;
}

esp_err_t argus_stepper_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "stepper not initialized");

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.run_commanded = false;
    s_ctx.emergency_stopped = false;
    argus_stepper_enable_driver(true);
    xSemaphoreGive(s_ctx.lock);

    ESP_LOGI(TAG, "stepper soft stop requested");
    return ESP_OK;
}

esp_err_t argus_stepper_emergency_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "stepper not initialized");

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.run_commanded = false;
    s_ctx.commanded_speed_hz = 0;
    s_ctx.emergency_stopped = true;
    argus_stepper_enable_driver(true);
    esp_err_t err = argus_set_applied_output_locked(0);
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "stepper emergency stopped and locked");
    }
    return err;
}

esp_err_t argus_stepper_unlock(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "stepper not initialized");

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.run_commanded = false;
    s_ctx.commanded_speed_hz = 0;
    s_ctx.emergency_stopped = false;
    esp_err_t err = argus_set_applied_output_locked(0);
    argus_stepper_enable_driver(false);
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stepper unlocked");
    }
    return err;
}

esp_err_t argus_stepper_get_status(argus_stepper_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is null");
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "stepper not initialized");

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    status->position_steps = argus_get_position_steps_locked();
    status->position_output_mrev = argus_position_steps_to_output_mrev(status->position_steps);
    status->commanded_speed_hz = s_ctx.commanded_speed_hz;
    status->applied_speed_hz = s_ctx.applied_speed_hz;
    status->commanded_rpm_milli = argus_steps_hz_to_output_rpm_milli(s_ctx.commanded_speed_hz);
    status->applied_rpm_milli = argus_steps_hz_to_output_rpm_milli(s_ctx.applied_speed_hz);
    status->run_commanded = s_ctx.run_commanded;
    status->motion_active = (s_ctx.applied_speed_hz != 0);
    status->direction_forward = s_ctx.direction_forward;
    status->enabled = s_ctx.enabled;
    status->driver_locked = s_ctx.enabled && (s_ctx.applied_speed_hz == 0);
    status->emergency_stopped = s_ctx.emergency_stopped;
    status->fault_code = 0U;
    xSemaphoreGive(s_ctx.lock);
    return ESP_OK;
}
