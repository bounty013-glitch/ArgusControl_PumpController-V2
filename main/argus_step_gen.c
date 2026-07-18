#include "argus_step_gen.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gptimer.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"

static const char *TAG = "argus_step_gen";

// Shared timing configuration structure
typedef struct {
    uint64_t integer;
    uint64_t remainder;
    uint64_t denominator;
    bool direction_forward;
    bool immediate_stop;
} argus_timing_settings_t;

// Static states - stored in internal DRAM (cache-safe)
static gptimer_handle_t s_timer = NULL;
static SemaphoreHandle_t s_lifecycle_mutex = NULL;

static volatile int32_t s_requested_rpm_milli = 0;
static volatile int32_t s_generated_rpm_milli = 0;

static volatile argus_timing_settings_t s_pending_timing = {0};
static argus_timing_settings_t s_active_timing = {0};
static portMUX_TYPE s_timing_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint64_t s_rem_accumulator = 0;
static volatile uint64_t s_next_step_ticks = 0;
static volatile int64_t s_step_count = 0;

static volatile bool s_forward = true;
static volatile bool s_running = false;
static volatile bool s_driver_enabled = false;
static volatile uint64_t s_last_deassertion_ticks = 0;
static volatile argus_step_gen_error_t s_error_state = ARGUS_STEP_GEN_ERROR_NONE;

// Internal DRAM-safe copies of configuration variables to avoid Flash/pointer dereferences in the ISR.
static gpio_num_t s_step_gpio = GPIO_NUM_NC;
static gpio_num_t s_dir_gpio = GPIO_NUM_NC;
static gpio_num_t s_en_gpio = GPIO_NUM_NC;

static int s_step_active_level = 0;
static int s_step_inactive_level = 1;
static int s_dir_forward_level = 0;
static int s_dir_reverse_level = 1;

// Timer alarm callback (IRAM safe)
static bool IRAM_ATTR argus_timer_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_data)
{
    (void)timer;
    (void)user_data;

    static bool pin_asserted = false;
    uint64_t current_alarm = edata->alarm_value;
    uint64_t next_alarm = current_alarm;

    if (pin_asserted) {
        // --- PHASE: DEASSERT_STEP (logical pulse end) ---
        // Drive STEP pin to logical inactive level (HIGH for common-anode)
        gpio_set_level(s_step_gpio, s_step_inactive_level);
        pin_asserted = false;
        s_last_deassertion_ticks = current_alarm; // Capture final deassertion edge timestamp for DIR hold timing

        // Apply pending direction changes when STEP is inactive
        taskENTER_CRITICAL_ISR(&s_timing_mux);
        if (s_pending_timing.direction_forward != s_forward) {
            s_forward = s_pending_timing.direction_forward;
            gpio_set_level(s_dir_gpio, s_forward ? s_dir_forward_level : s_dir_reverse_level);
        }
        taskEXIT_CRITICAL_ISR(&s_timing_mux);

        // Schedule logical STEP assertion (inactive interval = total ticks - active pulse width)
        // 15 us active pulse width = 150 ticks at 10 MHz resolution
        uint64_t low_duration = (s_next_step_ticks > 150ULL) ? (s_next_step_ticks - 150ULL) : 1ULL;
        next_alarm = current_alarm + low_duration;
    } else {
        // --- PHASE: ASSERT_STEP (logical pulse start) ---
        // Guard before STEP assertion edge execution
        if (!s_running || !s_driver_enabled) {
            gpio_set_level(s_step_gpio, s_step_inactive_level);
            return false;
        }

        // Apply rate updates safely at the start of a step
        taskENTER_CRITICAL_ISR(&s_timing_mux);
        if (s_pending_timing.immediate_stop) {
            s_active_timing.integer = 0;
            s_active_timing.remainder = 0;
            s_active_timing.denominator = 0;
            s_generated_rpm_milli = 0;
            s_pending_timing.immediate_stop = false;
        } else if (s_active_timing.integer != s_pending_timing.integer ||
                   s_active_timing.remainder != s_pending_timing.remainder ||
                   s_active_timing.denominator != s_pending_timing.denominator) {
            s_active_timing = s_pending_timing;
            s_generated_rpm_milli = s_requested_rpm_milli; // Accepted
            s_rem_accumulator = 0;                          // Reset accumulator for new rate
        }
        taskEXIT_CRITICAL_ISR(&s_timing_mux);

        if (s_active_timing.integer == 0ULL) {
            gpio_set_level(s_step_gpio, s_step_inactive_level);
            return false;
        }

        // Drive STEP pin to logical active level (LOW for common-anode)
        gpio_set_level(s_step_gpio, s_step_active_level);
        pin_asserted = true;

        // Bresenham accumulator to schedule exact timing intervals
        s_rem_accumulator += s_active_timing.remainder;
        uint64_t step_ticks = s_active_timing.integer;
        if (s_rem_accumulator >= s_active_timing.denominator) {
            step_ticks += 1;
            s_rem_accumulator -= s_active_timing.denominator;
        }
        s_next_step_ticks = step_ticks;

        // Count generated steps on logical assertion (inactive-to-active transition)
        if (s_forward) {
            s_step_count++;
        } else {
            s_step_count--;
        }

        // Schedule deassertion (150 ticks at 10 MHz = 15 us pulse width)
        next_alarm = current_alarm + 150ULL;
    }

    static gptimer_alarm_config_t local_alarm_config = {
        .alarm_count = 0,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = false,
    };
    
    // Safety clamp: Ensure the next alarm time is programmed in the future
    // to prevent missed matches due to interrupt latencies under network load.
    uint64_t now = 0;
    if (gptimer_get_raw_count(s_timer, &now) == ESP_OK) {
        if (next_alarm <= now + 30ULL) { // 3 us safety margin (30 ticks at 10 MHz)
            next_alarm = now + 30ULL;
        }
    }
    
    local_alarm_config.alarm_count = next_alarm;
    gptimer_set_alarm_action(s_timer, &local_alarm_config);
    return false;
}

esp_err_t argus_step_gen_init(const argus_config_t *cfg)
{
    if (cfg == NULL) {
        s_error_state = ARGUS_STEP_GEN_ERROR_INIT_FAILED;
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize lifecycle mutex for serialization of task-context start/stop/arm
    if (s_lifecycle_mutex == NULL) {
        s_lifecycle_mutex = xSemaphoreCreateMutex();
        if (s_lifecycle_mutex == NULL) {
            s_error_state = ARGUS_STEP_GEN_ERROR_INIT_FAILED;
            return ESP_ERR_NO_MEM;
        }
    }

    s_step_gpio = cfg->step_gpio;
    s_dir_gpio = cfg->dir_gpio;
    s_en_gpio = cfg->en_gpio;

    // Set configuration-defined polarities
    s_step_active_level = cfg->step_active_low ? 0 : 1;
    s_step_inactive_level = cfg->step_active_low ? 1 : 0;
    s_dir_forward_level = cfg->dir_inverted ? 0 : 1;
    s_dir_reverse_level = cfg->dir_inverted ? 1 : 0;

    s_requested_rpm_milli = 0;
    s_generated_rpm_milli = 0;
    s_rem_accumulator = 0;
    s_next_step_ticks = 0;
    s_step_count = 0;
    s_forward = true;
    s_running = false;
    s_driver_enabled = false;
    s_last_deassertion_ticks = 0;
    s_error_state = ARGUS_STEP_GEN_ERROR_NONE;

    // Configure outputs STEP and DIR with input readback enabled for tests
    gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << s_step_gpio) | (1ULL << s_dir_gpio),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_config) != ESP_OK) {
        s_error_state = ARGUS_STEP_GEN_ERROR_INIT_FAILED;
        return ESP_FAIL;
    }

    // Force STEP inactive-high and DIR to forward level immediately on boot
    gpio_set_level(s_step_gpio, s_step_inactive_level);
    gpio_set_level(s_dir_gpio, s_dir_forward_level);

    // Glitch-Free ENA Initialization Sequence:
    // 1. Preload GPIO 5 latch to HIGH (Disabled/unlocked state)
    gpio_set_level(s_en_gpio, 1);

    // 2. Configure GPIO 5 as output with input readback enabled for tests
    gpio_config_t en_io_config = {
        .pin_bit_mask = (1ULL << s_en_gpio),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&en_io_config) != ESP_OK) {
        // Enforce safe disabled levels on init fail
        gpio_set_level(s_step_gpio, s_step_inactive_level);
        gpio_set_level(s_en_gpio, 1);
        s_error_state = ARGUS_STEP_GEN_ERROR_INIT_FAILED;
        return ESP_FAIL;
    }

    // 3. Keep driver disabled initially
    s_driver_enabled = false;

    // Initializing GPTimer
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 10000000, // 10 MHz resolution (1 tick = 100 ns)
    };
    if (gptimer_new_timer(&timer_config, &s_timer) != ESP_OK) {
        gpio_set_level(s_step_gpio, s_step_inactive_level);
        gpio_set_level(s_en_gpio, 1);
        s_error_state = ARGUS_STEP_GEN_ERROR_INIT_FAILED;
        return ESP_FAIL;
    }

    static gptimer_alarm_config_t alarm_config = {
        .alarm_count = 10000ULL,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = false,
    };
    if (gptimer_set_alarm_action(s_timer, &alarm_config) != ESP_OK) {
        gpio_set_level(s_step_gpio, s_step_inactive_level);
        gpio_set_level(s_en_gpio, 1);
        s_error_state = ARGUS_STEP_GEN_ERROR_INIT_FAILED;
        return ESP_FAIL;
    }

    gptimer_event_callbacks_t cbs = {
        .on_alarm = argus_timer_alarm_cb,
    };
    if (gptimer_register_event_callbacks(s_timer, &cbs, NULL) != ESP_OK) {
        gpio_set_level(s_step_gpio, s_step_inactive_level);
        gpio_set_level(s_en_gpio, 1);
        s_error_state = ARGUS_STEP_GEN_ERROR_INIT_FAILED;
        return ESP_FAIL;
    }
    
    if (gptimer_enable(s_timer) != ESP_OK) {
        gpio_set_level(s_step_gpio, s_step_inactive_level);
        gpio_set_level(s_en_gpio, 1);
        s_error_state = ARGUS_STEP_GEN_ERROR_INIT_FAILED;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GPTimer step generator initialized (resolution=10MHz, step_gpio=%d, dir_gpio=%d, en_gpio=%d as Active-Low Output)", 
             s_step_gpio, s_dir_gpio, s_en_gpio);
    return ESP_OK;
}

esp_err_t argus_step_gen_arm(void)
{
    if (s_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    s_requested_rpm_milli = 0;
    s_generated_rpm_milli = 0;
    s_running = false;
    
    taskENTER_CRITICAL(&s_timing_mux);
    s_pending_timing.integer = 0;
    s_pending_timing.remainder = 0;
    s_pending_timing.denominator = 0;
    s_pending_timing.direction_forward = s_forward;
    s_pending_timing.immediate_stop = false;
    s_active_timing = s_pending_timing;
    taskEXIT_CRITICAL(&s_timing_mux);

    esp_err_t err = gptimer_start(s_timer);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    } else if (err != ESP_OK) {
        gpio_set_level(s_step_gpio, s_step_inactive_level);
        s_error_state = ARGUS_STEP_GEN_ERROR_TIMING_UPDATE_FAILED;
        ESP_LOGE(TAG, "arm: gptimer_start failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lifecycle_mutex);
    return err;
}

esp_err_t argus_step_gen_enable_driver(void)
{
    if (s_en_gpio == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_driver_enabled) {
        // Drive GPIO 5 LOW to energize ENA optocoupler (Active-Low)
        gpio_set_level(s_en_gpio, 0);
        s_driver_enabled = true;
        ESP_LOGI(TAG, "Driver ENABLED (GPIO 5 LOW). Waiting 20 ms settle time.");
        // Wait for the required 20 ms enable-settle delay
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_OK;
}

esp_err_t argus_step_gen_disable_driver(void)
{
    if (s_en_gpio == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Stop active stepping before disabling
    taskENTER_CRITICAL(&s_timing_mux);
    s_running = false;
    s_requested_rpm_milli = 0;
    s_generated_rpm_milli = 0;
    s_pending_timing.integer = 0;
    s_pending_timing.remainder = 0;
    s_pending_timing.denominator = 0;
    s_pending_timing.immediate_stop = true;
    s_active_timing = (argus_timing_settings_t){0};
    taskEXIT_CRITICAL(&s_timing_mux);

    if (s_timer != NULL) {
        gptimer_stop(s_timer);
    }
    gpio_set_level(s_step_gpio, s_step_inactive_level); // Force STEP inactive-high immediately

    // Drive GPIO 5 HIGH to disable driver (Active-Low) and release shaft
    gpio_set_level(s_en_gpio, 1);
    s_driver_enabled = false;
    s_error_state = ARGUS_STEP_GEN_ERROR_NONE;
    ESP_LOGI(TAG, "Driver DISABLED/UNLOCKED (GPIO 5 HIGH). Holding torque released.");

    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_OK;
}

bool argus_step_gen_is_driver_enabled(void)
{
    return s_driver_enabled;
}

esp_err_t argus_step_gen_set_direction(bool forward)
{
    if (xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    taskENTER_CRITICAL(&s_timing_mux);
    // Reject DIR change if actively generating non-zero speed
    if (s_running && s_active_timing.integer > 0) {
        taskEXIT_CRITICAL(&s_timing_mux);
        xSemaphoreGive(s_lifecycle_mutex);
        ESP_LOGE(TAG, "Rejecting direction change: step generation is active.");
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t last_deassertion = s_last_deassertion_ticks;
    taskEXIT_CRITICAL(&s_timing_mux);

    // Enforce DIR hold time following the final STEP deassertion edge (return to inactive HIGH).
    if (s_timer != NULL) {
        uint64_t count = 0;
        esp_err_t err = gptimer_get_raw_count(s_timer, &count);
        if (err == ESP_OK) {
            uint64_t diff = count - last_deassertion;
            if (diff < 50ULL) { // 50 ticks = 5 us hold time at 10 MHz
                esp_rom_delay_us(5);
            }
        } else {
            // Timer is stopped or inactive; enforce conservative 5 us DIR hold delay
            esp_rom_delay_us(5);
        }
    }

    taskENTER_CRITICAL(&s_timing_mux);
    s_pending_timing.direction_forward = forward;
    s_forward = forward;
    gpio_set_level(s_dir_gpio, forward ? s_dir_forward_level : s_dir_reverse_level);
    taskEXIT_CRITICAL(&s_timing_mux);

    // Enforce DIR setup delay (DIR must be stable at least 5 us before next STEP assertion edge)
    esp_rom_delay_us(5);
    
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_OK;
}

esp_err_t argus_step_gen_set_rate_rpm_milli(int32_t rpm_milli, uint32_t steps_per_rev)
{
    if (s_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (rpm_milli == 0) {
        taskENTER_CRITICAL(&s_timing_mux);
        s_requested_rpm_milli = 0;
        s_pending_timing.integer = 0;
        s_pending_timing.remainder = 0;
        s_pending_timing.denominator = 0;
        taskEXIT_CRITICAL(&s_timing_mux);
        gpio_set_level(s_step_gpio, s_step_inactive_level); // Force STEP inactive-high on zero rate
        return ESP_OK;
    }

    if (steps_per_rev == 0) {
        s_error_state = ARGUS_STEP_GEN_ERROR_TIMING_UPDATE_FAILED;
        ESP_LOGE(TAG, "set_rate: steps_per_rev must be > 0");
        return ESP_ERR_INVALID_ARG;
    }

    // Min: 1 milli-RPM (0.001 RPM), Max: 200 RPM (200,000 milli-RPM)
    if (rpm_milli < 1 || rpm_milli > 200000) {
        s_error_state = ARGUS_STEP_GEN_ERROR_TIMING_UPDATE_FAILED;
        ESP_LOGE(TAG, "set_rate: rpm_milli (%ld) out of range [1, 200000]", (long)rpm_milli);
        return ESP_ERR_INVALID_ARG;
    }

    // Enable-Before-Motion Ordering:
    // Automatically enable driver and wait 20 ms before setting the target rate
    if (!s_driver_enabled) {
        esp_err_t err = argus_step_gen_enable_driver();
        if (err != ESP_OK) {
            return err;
        }
    }

    // Exact rational period formula: Ticks = 10,000,000 * 60,000 / (rpm_milli * steps_per_rev)
    uint64_t num = 600000000000ULL;
    uint64_t den = (uint64_t)rpm_milli * (uint64_t)steps_per_rev;
    uint64_t integer = num / den;
    uint64_t remainder = num % den;

    // Boundary constraint validation:
    // STEP active-low duration is 15 us (150 ticks). Minimum inactive-high duration is 15 us (150 ticks).
    // Total minimum period is 30 us (300 ticks).
    if (integer < 300ULL) {
        s_error_state = ARGUS_STEP_GEN_ERROR_TIMING_UPDATE_FAILED;
        ESP_LOGE(TAG, "set_rate: period ticks (%lld) < 300 min ticks constraint", (long long)integer);
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_timing_mux);
    s_requested_rpm_milli = rpm_milli;
    s_pending_timing.integer = integer;
    s_pending_timing.remainder = remainder;
    s_pending_timing.denominator = den;
    s_pending_timing.immediate_stop = false;
    taskEXIT_CRITICAL(&s_timing_mux);

    // Kickstart pulse stream if moving from 0 speed while running
    if (s_generated_rpm_milli == 0 && s_running) {
        esp_err_t start_err = gptimer_start(s_timer);
        if (start_err != ESP_OK && start_err != ESP_ERR_INVALID_STATE) {
            s_error_state = ARGUS_STEP_GEN_ERROR_TIMING_UPDATE_FAILED;
            ESP_LOGE(TAG, "kickstart: gptimer_start failed: %s", esp_err_to_name(start_err));
            return start_err;
        }

        uint64_t count = 0;
        gptimer_get_raw_count(s_timer, &count);

        taskENTER_CRITICAL(&s_timing_mux);
        s_active_timing = s_pending_timing;
        s_generated_rpm_milli = rpm_milli;
        s_rem_accumulator = 0;
        s_forward = s_pending_timing.direction_forward;
        taskEXIT_CRITICAL(&s_timing_mux);

        // Apply setup timing for DIR before rising edge (5 us delay)
        gpio_set_level(s_dir_gpio, s_forward ? s_dir_forward_level : s_dir_reverse_level);
        esp_rom_delay_us(5);

        // First pulse logical assertion (drives active-low level)
        gpio_set_level(s_step_gpio, s_step_active_level);
        s_next_step_ticks = s_active_timing.integer;

        if (s_forward) {
            s_step_count++;
        } else {
            s_step_count--;
        }

        static gptimer_alarm_config_t alarm_config = {
            .alarm_count = 0,
            .reload_count = 0,
            .flags.auto_reload_on_alarm = false,
        };
        alarm_config.alarm_count = count + 150ULL; // deassertion in 15 us
        gptimer_set_alarm_action(s_timer, &alarm_config);
    }
    s_error_state = ARGUS_STEP_GEN_ERROR_NONE;
    return ESP_OK;
}

esp_err_t argus_step_gen_start(void)
{
    if (s_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_running = true;
    esp_err_t err = gptimer_start(s_timer);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    xSemaphoreGive(s_lifecycle_mutex);
    return err;
}

esp_err_t argus_step_gen_stop_immediate(void)
{
    if (s_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    taskENTER_CRITICAL(&s_timing_mux);
    bool was_running = s_running;
    s_running = false;
    s_requested_rpm_milli = 0;
    s_generated_rpm_milli = 0;

    s_pending_timing.integer = 0;
    s_pending_timing.remainder = 0;
    s_pending_timing.denominator = 0;
    s_pending_timing.immediate_stop = true;
    s_active_timing = (argus_timing_settings_t){0};
    taskEXIT_CRITICAL(&s_timing_mux);

    esp_err_t err = ESP_OK;
    if (was_running) {
        err = gptimer_stop(s_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Idempotent stop failed: %s", esp_err_to_name(err));
            s_error_state = ARGUS_STEP_GEN_ERROR_TIMING_UPDATE_FAILED;
        }
    }

    // Force STEP pin to inactive-high immediately
    gpio_set_level(s_step_gpio, s_step_inactive_level);

    // Capture stop deassertion timestamp
    if (s_timer != NULL) {
        uint64_t count = 0;
        if (gptimer_get_raw_count(s_timer, &count) == ESP_OK) {
            taskENTER_CRITICAL(&s_timing_mux);
            s_last_deassertion_ticks = count;
            taskEXIT_CRITICAL(&s_timing_mux);
        }
    }

    s_error_state = ARGUS_STEP_GEN_ERROR_NONE;
    xSemaphoreGive(s_lifecycle_mutex);
    return err;
}

esp_err_t argus_step_gen_stop_profiled(void)
{
    taskENTER_CRITICAL(&s_timing_mux);
    s_requested_rpm_milli = 0;
    s_pending_timing.integer = 0;
    s_pending_timing.remainder = 0;
    s_pending_timing.denominator = 0;
    taskEXIT_CRITICAL(&s_timing_mux);
    s_error_state = ARGUS_STEP_GEN_ERROR_NONE;
    return ESP_OK;
}

int32_t argus_step_gen_get_requested_rpm_milli(void)
{
    return s_requested_rpm_milli;
}

int32_t argus_step_gen_get_generated_rpm_milli(void)
{
    return s_generated_rpm_milli;
}

int64_t argus_step_gen_get_step_count(void)
{
    taskENTER_CRITICAL(&s_timing_mux);
    int64_t count = s_step_count;
    taskEXIT_CRITICAL(&s_timing_mux);
    return count;
}

argus_step_gen_error_t argus_step_gen_get_error(void)
{
    return s_error_state;
}

void argus_step_gen_clear_error(void)
{
    s_error_state = ARGUS_STEP_GEN_ERROR_NONE;
}

void argus_step_gen_reset_step_count(void)
{
    taskENTER_CRITICAL(&s_timing_mux);
    s_step_count = 0;
    taskEXIT_CRITICAL(&s_timing_mux);
}

void argus_step_gen_get_snapshot(argus_step_gen_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;

    taskENTER_CRITICAL(&s_timing_mux);
    snapshot->requested_rpm_milli = s_requested_rpm_milli;
    snapshot->generated_rpm_milli = s_generated_rpm_milli;
    snapshot->generated_step_count = s_step_count;
    snapshot->driver_enabled = s_driver_enabled;
    snapshot->is_running = s_running;
    snapshot->is_forward = s_forward;
    snapshot->error_state = s_error_state;
    taskEXIT_CRITICAL(&s_timing_mux);
}
