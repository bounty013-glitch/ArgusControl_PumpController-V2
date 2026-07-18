#include "argus_trajectory.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "argus_conversions.h"
#include "argus_step_gen.h"

static const char *TAG = "argus_trajectory";

static const argus_config_t *s_cfg = NULL;
static SemaphoreHandle_t s_traj_mutex = NULL;
static TaskHandle_t s_task_handle = NULL;

static int32_t s_target_rpm_milli = 0;
static int32_t s_applied_rpm_milli = 0;

static bool s_target_forward = true;
static bool s_current_forward = true;

static bool s_ramp_active = false;
static bool s_is_reversing = false;
static uint32_t s_latched_error = 0;

static int32_t s_accel_limit = 10000; // 10.0 RPM/s
static int32_t s_decel_limit = 10000; // 10.0 RPM/s
static uint32_t s_update_interval_ms = 20;

esp_err_t argus_trajectory_init(const argus_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;
    s_accel_limit = cfg->accel_milli_rpm_per_sec;
    s_decel_limit = cfg->decel_milli_rpm_per_sec;
    s_update_interval_ms = 20;

    if (s_traj_mutex == NULL) {
        s_traj_mutex = xSemaphoreCreateMutex();
        if (s_traj_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_traj_mutex, portMAX_DELAY);
    s_target_rpm_milli = 0;
    s_applied_rpm_milli = 0;
    s_target_forward = true;
    s_current_forward = true;
    s_ramp_active = false;
    s_is_reversing = false;
    xSemaphoreGive(s_traj_mutex);

    ESP_LOGI(TAG, "Trajectory engine initialized (accel=%ld mRPM/s, decel=%ld mRPM/s, interval=%ld ms)",
             (long)s_accel_limit, (long)s_decel_limit, (long)s_update_interval_ms);
    return ESP_OK;
}

void argus_trajectory_step_tick_20ms(void)
{
    if (s_cfg == NULL) return;

    if (s_traj_mutex != NULL) {
        xSemaphoreTake(s_traj_mutex, portMAX_DELAY);
    }

    int32_t target = s_target_rpm_milli;
    int32_t applied = s_applied_rpm_milli;
    bool target_fwd = s_target_forward;
    bool current_fwd = s_current_forward;
    bool reversing = s_is_reversing;

    uint32_t spr = argus_conversions_steps_per_rev(s_cfg);

    // Delta per 20 ms update (Fixed-point integer division)
    // 10,000 mRPM/sec * 20 ms / 1000 ms = 200 mRPM
    int32_t delta_accel = (s_accel_limit * (int32_t)s_update_interval_ms) / 1000;
    int32_t delta_decel = (s_decel_limit * (int32_t)s_update_interval_ms) / 1000;
    if (delta_accel < 1) delta_accel = 1;
    if (delta_decel < 1) delta_decel = 1;

    // --- 1. Handle Direction Reversal Ramp Through Zero ---
    if (target_fwd != current_fwd || reversing) {
        if (applied > 0) {
            s_is_reversing = true;
            s_ramp_active = true;
            applied -= delta_decel;
            if (applied <= 0) {
                applied = 0;
                argus_step_gen_stop_immediate();
                argus_step_gen_set_direction(target_fwd);
                s_current_forward = target_fwd;
                s_is_reversing = false;
            }
        } else {
            // Applied is zero, complete direction switch
            argus_step_gen_stop_immediate();
            argus_step_gen_set_direction(target_fwd);
            s_current_forward = target_fwd;
            s_is_reversing = false;
        }
    } else {
        // --- 2. Standard Target Ramp Acceleration / Deceleration ---
        if (applied < target) {
            s_ramp_active = true;
            applied += delta_accel;
            if (applied >= target) {
                applied = target;
                s_ramp_active = false;
            }
        } else if (applied > target) {
            s_ramp_active = true;
            applied -= delta_decel;
            if (applied <= target) {
                applied = target;
                s_ramp_active = false;
            }
        } else {
            s_ramp_active = false;
        }
    }

    s_applied_rpm_milli = applied;

    // --- 3. Apply Speed to GPTimer Pulse Engine ---
    if (applied > 0) {
        // Automatically enable driver (drives ENA LOW and waits 20 ms if not already enabled)
        if (!argus_step_gen_is_driver_enabled()) {
            argus_step_gen_enable_driver();
        }
        argus_step_gen_start();
        argus_step_gen_set_rate_rpm_milli(applied, spr);
    } else if (applied == 0) {
        if (argus_step_gen_get_generated_rpm_milli() > 0) {
            argus_step_gen_stop_profiled();
        }
    }

    if (s_traj_mutex != NULL) {
        xSemaphoreGive(s_traj_mutex);
    }
}

static void argus_trajectory_task(void *arg)
{
    (void)arg;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(20);

    while (true) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        argus_trajectory_step_tick_20ms();
    }
}

esp_err_t argus_trajectory_task_start(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK; // Already running
    }
    BaseType_t res = xTaskCreate(argus_trajectory_task, "traj_task", 4096, NULL, 6, &s_task_handle);
    return (res == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t argus_trajectory_set_target_rpm_milli(int32_t rpm_milli, bool forward)
{
    if (s_cfg == NULL) return ESP_ERR_INVALID_STATE;

    if (rpm_milli < 0) rpm_milli = 0;
    if (rpm_milli > s_cfg->max_output_milli_rpm) {
        rpm_milli = s_cfg->max_output_milli_rpm;
    }

    if (s_traj_mutex != NULL) {
        xSemaphoreTake(s_traj_mutex, portMAX_DELAY);
    }

    s_target_rpm_milli = rpm_milli;
    s_target_forward = forward;
    if (s_target_forward != s_current_forward && s_applied_rpm_milli > 0) {
        s_is_reversing = true;
    }

    // If starting from zero and driver is disabled, enable driver with 20 ms settle
    if (s_applied_rpm_milli == 0 && rpm_milli > 0) {
        if (!argus_step_gen_is_driver_enabled()) {
            argus_step_gen_enable_driver();
        }
    }

    if (s_traj_mutex != NULL) {
        xSemaphoreGive(s_traj_mutex);
    }

    return ESP_OK;
}

esp_err_t argus_trajectory_stop_normal(void)
{
    return argus_trajectory_set_target_rpm_milli(0, s_target_forward);
}

esp_err_t argus_trajectory_stop_immediate(void)
{
    if (s_traj_mutex != NULL) {
        xSemaphoreTake(s_traj_mutex, portMAX_DELAY);
    }

    s_target_rpm_milli = 0;
    s_applied_rpm_milli = 0;
    s_ramp_active = false;
    s_is_reversing = false;

    argus_step_gen_stop_immediate(); // Halts pulses immediately, retains holding torque (ENA LOW)

    if (s_traj_mutex != NULL) {
        xSemaphoreGive(s_traj_mutex);
    }

    return ESP_OK;
}

esp_err_t argus_trajectory_unlock(void)
{
    if (s_traj_mutex != NULL) {
        xSemaphoreTake(s_traj_mutex, portMAX_DELAY);
    }

    s_target_rpm_milli = 0;
    s_applied_rpm_milli = 0;
    s_ramp_active = false;
    s_is_reversing = false;

    argus_step_gen_disable_driver(); // Halts pulses immediately, drives ENA HIGH (releases shaft)

    if (s_traj_mutex != NULL) {
        xSemaphoreGive(s_traj_mutex);
    }

    return ESP_OK;
}

esp_err_t argus_trajectory_recover(void)
{
    if (s_traj_mutex != NULL) {
        xSemaphoreTake(s_traj_mutex, portMAX_DELAY);
    }

    s_target_rpm_milli = 0;
    s_applied_rpm_milli = 0;
    s_ramp_active = false;
    s_is_reversing = false;

    // 1. Immediately terminate STEP generation
    esp_err_t err_stop = argus_step_gen_stop_immediate();

    // 2. Drive ENA HIGH to disable UIM
    esp_err_t err_dis = argus_step_gen_disable_driver();

    argus_trajectory_clear_error();

    if (s_traj_mutex != NULL) {
        xSemaphoreGive(s_traj_mutex);
    }

    if (err_stop != ESP_OK || err_dis != ESP_OK) {
        ESP_LOGE(TAG, "Recovery failed: lower-layer stop/disable returned error");
        return (err_stop != ESP_OK) ? err_stop : err_dis;
    }

    ESP_LOGW(TAG, "Diagnostic RECOVERY sequence triggered. Driver unlocked. Waiting 500 ms recovery interval...");

    // 3. Wait 500 ms recovery interval
    vTaskDelay(pdMS_TO_TICKS(500));

    // 4. Verify fresh lower-layer snapshot error state
    argus_step_gen_snapshot_t step_snap;
    argus_step_gen_get_snapshot(&step_snap);
    if (step_snap.error_state != ARGUS_STEP_GEN_ERROR_NONE) {
        ESP_LOGE(TAG, "Recovery failed: step generator error state remains non-zero (%d)", (int)step_snap.error_state);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Software recovery complete. Software state restored to UNLOCKED.");
    return ESP_OK;
}

void argus_trajectory_clear_error(void)
{
    if (s_traj_mutex != NULL) {
        xSemaphoreTake(s_traj_mutex, portMAX_DELAY);
    }
    s_latched_error = 0;
    argus_step_gen_clear_error();
    if (s_traj_mutex != NULL) {
        xSemaphoreGive(s_traj_mutex);
    }
}

esp_err_t argus_trajectory_enable_driver(void)
{
    if (s_traj_mutex != NULL) {
        xSemaphoreTake(s_traj_mutex, portMAX_DELAY);
    }
    esp_err_t err = argus_step_gen_enable_driver();
    if (s_traj_mutex != NULL) {
        xSemaphoreGive(s_traj_mutex);
    }
    return err;
}

int32_t argus_trajectory_get_target_rpm_milli(void)
{
    return s_target_rpm_milli;
}

int32_t argus_trajectory_get_applied_rpm_milli(void)
{
    return s_applied_rpm_milli;
}

bool argus_trajectory_is_ramp_active(void)
{
    return s_ramp_active;
}

bool argus_trajectory_is_reversing(void)
{
    return s_is_reversing || (s_target_forward != s_current_forward && s_applied_rpm_milli > 0);
}

void argus_trajectory_get_snapshot(argus_trajectory_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;

    if (s_traj_mutex != NULL) {
        xSemaphoreTake(s_traj_mutex, portMAX_DELAY);
    }

    argus_step_gen_snapshot_t step_snap;
    argus_step_gen_get_snapshot(&step_snap);

    snapshot->target_rpm_milli = s_target_rpm_milli;
    snapshot->applied_rpm_milli = s_applied_rpm_milli;
    snapshot->generated_rpm_milli = step_snap.generated_rpm_milli;
    snapshot->target_forward = s_target_forward;
    snapshot->applied_forward = s_current_forward;
    snapshot->ramp_active = s_ramp_active;
    snapshot->is_reversing = s_is_reversing;
    snapshot->latched_error = (uint32_t)step_snap.error_state;

    if (s_traj_mutex != NULL) {
        xSemaphoreGive(s_traj_mutex);
    }
}
