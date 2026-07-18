#include "argus_state_mgr.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "argus_trajectory.h"
#include "argus_step_gen.h"

static const char *TAG = "argus_state_mgr";

// Default production operations table binding directly to argus_trajectory and argus_step_gen
static esp_err_t prod_enable_driver(void) { return argus_trajectory_enable_driver(); }
static esp_err_t prod_disable_driver(void) { return argus_trajectory_unlock(); }
static esp_err_t prod_set_direction(bool fwd) { return argus_step_gen_set_direction(fwd); }
static esp_err_t prod_set_target_rate(int32_t rpm, bool fwd) { return argus_trajectory_set_target_rpm_milli(rpm, fwd); }
static esp_err_t prod_stop_immediate(void) { return argus_trajectory_stop_immediate(); }
static esp_err_t prod_stop_normal(void) { return argus_trajectory_stop_normal(); }
static esp_err_t prod_recover(void) { return argus_trajectory_recover(); }
static uint32_t prod_get_error(void) {
    argus_trajectory_snapshot_t snap;
    argus_trajectory_get_snapshot(&snap);
    return snap.latched_error;
}

static const argus_motion_ops_t s_production_ops = {
    .enable_driver = prod_enable_driver,
    .disable_driver = prod_disable_driver,
    .set_direction = prod_set_direction,
    .set_target_rate = prod_set_target_rate,
    .stop_immediate = prod_stop_immediate,
    .stop_normal = prod_stop_normal,
    .recover = prod_recover,
    .get_error = prod_get_error,
};

// Live production core instance and synchronization primitives
static argus_state_core_t s_prod_core;
static SemaphoreHandle_t s_command_mutex = NULL; // Serializes state-manager commands
static SemaphoreHandle_t s_state_mutex = NULL;   // Guards state core variables (short critical sections only)
static TaskHandle_t s_evaluator_task_handle = NULL;

const char *argus_state_mgr_get_state_name(argus_machine_state_t state)
{
    switch (state) {
        case ARGUS_STATE_BOOTING:           return "BOOTING";
        case ARGUS_STATE_UNLOCKED:          return "UNLOCKED";
        case ARGUS_STATE_STARTING:          return "STARTING";
        case ARGUS_STATE_RUNNING:           return "RUNNING";
        case ARGUS_STATE_DECELERATING:      return "DECELERATING";
        case ARGUS_STATE_HOLDING:           return "HOLDING";
        case ARGUS_STATE_EMERGENCY_STOPPED: return "EMERGENCY_STOPPED";
        case ARGUS_STATE_RECOVERING:        return "RECOVERING";
        case ARGUS_STATE_FAULTED:           return "FAULTED";
        default:                            return "UNKNOWN";
    }
}

static void set_core_rejection_reason(argus_state_core_t *core, const char *reason)
{
    if (core != NULL && reason != NULL) {
        snprintf(core->last_rejection_reason, sizeof(core->last_rejection_reason), "%s", reason);
        ESP_LOGW(TAG, "Command rejected: %s", reason);
    }
}

// ================= PURE STATE CORE TRANSITION FUNCTIONS =================

void argus_state_core_init(argus_state_core_t *core, const argus_config_t *cfg, const argus_motion_ops_t *ops)
{
    (void)cfg;
    if (core == NULL) return;

    memset(core, 0, sizeof(argus_state_core_t));
    core->ops = (ops != NULL) ? ops : &s_production_ops;
    core->machine_state = ARGUS_STATE_UNLOCKED; // Transition: BOOTING -> UNLOCKED
    core->configured_target_rpm_milli = 0;
    core->requested_forward = true;
    core->estop_latched = false;
    core->estop_reset_destination = ARGUS_STATE_HOLDING;
    core->fault_code = 0;
    core->command_generation = 1;
    snprintf(core->last_rejection_reason, sizeof(core->last_rejection_reason), "None");
}

esp_err_t argus_state_core_set_target(argus_state_core_t *core, int32_t rpm_milli, bool forward)
{
    if (core == NULL) return ESP_ERR_INVALID_ARG;

    // Permission check
    if (core->estop_latched || core->machine_state == ARGUS_STATE_EMERGENCY_STOPPED) {
        set_core_rejection_reason(core, "Cannot set target while EMERGENCY_STOPPED");
        return ESP_ERR_INVALID_STATE;
    }
    if (core->machine_state == ARGUS_STATE_FAULTED) {
        set_core_rejection_reason(core, "Cannot set target while FAULTED");
        return ESP_ERR_INVALID_STATE;
    }
    if (core->machine_state == ARGUS_STATE_DECELERATING) {
        set_core_rejection_reason(core, "Cannot set target while DECELERATING");
        return ESP_ERR_INVALID_STATE;
    }
    if (core->machine_state == ARGUS_STATE_RECOVERING || core->machine_state == ARGUS_STATE_BOOTING) {
        set_core_rejection_reason(core, "Cannot set target during BOOTING/RECOVERING");
        return ESP_ERR_INVALID_STATE;
    }

    // Range check: [0, 200000] mRPM (0 to 200 RPM)
    if (rpm_milli < 0 || rpm_milli > 200000) {
        set_core_rejection_reason(core, "Target RPM out of range [0, 200000]");
        return ESP_ERR_INVALID_ARG;
    }

    core->configured_target_rpm_milli = rpm_milli;
    core->requested_forward = forward;

    // If moving, apply target rate to motion engine
    if (core->machine_state == ARGUS_STATE_STARTING || core->machine_state == ARGUS_STATE_RUNNING) {
        if (core->ops != NULL && core->ops->set_target_rate != NULL) {
            esp_err_t err = core->ops->set_target_rate(rpm_milli, forward);
            if (err != ESP_OK) {
                core->machine_state = ARGUS_STATE_FAULTED;
                core->fault_code = err;
                set_core_rejection_reason(core, "set_target_rate failed");
                return err;
            }
        }
    }

    return ESP_OK;
}

esp_err_t argus_state_core_start(argus_state_core_t *core)
{
    if (core == NULL) return ESP_ERR_INVALID_ARG;

    if (core->estop_latched || core->machine_state == ARGUS_STATE_EMERGENCY_STOPPED) {
        set_core_rejection_reason(core, "Cannot start while EMERGENCY_STOPPED");
        return ESP_ERR_INVALID_STATE;
    }
    if (core->machine_state == ARGUS_STATE_FAULTED) {
        set_core_rejection_reason(core, "Cannot start while FAULTED");
        return ESP_ERR_INVALID_STATE;
    }
    if (core->machine_state == ARGUS_STATE_DECELERATING) {
        set_core_rejection_reason(core, "Cannot start while DECELERATING");
        return ESP_ERR_INVALID_STATE;
    }
    if (core->machine_state == ARGUS_STATE_STARTING || core->machine_state == ARGUS_STATE_RUNNING) {
        return ESP_OK; // Idempotent start
    }
    if (core->configured_target_rpm_milli <= 0) {
        set_core_rejection_reason(core, "Cannot start with zero setpoint");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t entry_gen = core->command_generation;
    int32_t target = core->configured_target_rpm_milli;
    bool fwd = core->requested_forward;

    core->machine_state = ARGUS_STATE_STARTING;

    // Execute driver enable
    esp_err_t err = ESP_OK;
    if (core->ops != NULL && core->ops->enable_driver != NULL) {
        err = core->ops->enable_driver();
    }

    // Revalidate generation counter after potential delay/enable
    if (core->command_generation != entry_gen || core->estop_latched || core->machine_state == ARGUS_STATE_EMERGENCY_STOPPED) {
        set_core_rejection_reason(core, "Start aborted: E-stop latched during driver enable");
        return ESP_ERR_INVALID_STATE;
    }

    if (err == ESP_OK && core->ops != NULL && core->ops->set_target_rate != NULL) {
        err = core->ops->set_target_rate(target, fwd);
    }

    // Final generation revalidation
    if (core->command_generation != entry_gen || core->estop_latched || core->machine_state == ARGUS_STATE_EMERGENCY_STOPPED) {
        set_core_rejection_reason(core, "Start aborted: E-stop latched during rate set");
        return ESP_ERR_INVALID_STATE;
    }

    if (err != ESP_OK) {
        core->machine_state = ARGUS_STATE_FAULTED;
        core->fault_code = err;
        set_core_rejection_reason(core, "Lower layer enable/rate call failed during start");
    }

    return err;
}

esp_err_t argus_state_core_stop_normal(argus_state_core_t *core)
{
    if (core == NULL) return ESP_ERR_INVALID_ARG;

    if (core->machine_state == ARGUS_STATE_EMERGENCY_STOPPED || core->machine_state == ARGUS_STATE_FAULTED) {
        set_core_rejection_reason(core, "Cannot stop_normal while EMERGENCY_STOPPED/FAULTED");
        return ESP_ERR_INVALID_STATE;
    }

    if (core->machine_state == ARGUS_STATE_UNLOCKED || core->machine_state == ARGUS_STATE_HOLDING || core->machine_state == ARGUS_STATE_DECELERATING) {
        return ESP_OK; // Idempotent stop
    }

    core->machine_state = ARGUS_STATE_DECELERATING;

    esp_err_t err = ESP_OK;
    if (core->ops != NULL && core->ops->stop_normal != NULL) {
        err = core->ops->stop_normal();
    }

    if (err != ESP_OK) {
        core->machine_state = ARGUS_STATE_FAULTED;
        core->fault_code = err;
        set_core_rejection_reason(core, "stop_normal lower-layer call failed");
    }

    return err;
}

esp_err_t argus_state_core_unlock(argus_state_core_t *core)
{
    if (core == NULL) return ESP_ERR_INVALID_ARG;

    if (core->machine_state == ARGUS_STATE_STARTING || core->machine_state == ARGUS_STATE_RUNNING || core->machine_state == ARGUS_STATE_DECELERATING) {
        set_core_rejection_reason(core, "Cannot unlock while motion is active/decelerating");
        return ESP_ERR_INVALID_STATE;
    }
    if (core->machine_state == ARGUS_STATE_EMERGENCY_STOPPED || core->machine_state == ARGUS_STATE_FAULTED) {
        set_core_rejection_reason(core, "Cannot unlock while EMERGENCY_STOPPED/FAULTED");
        return ESP_ERR_INVALID_STATE;
    }
    if (core->machine_state == ARGUS_STATE_UNLOCKED) {
        return ESP_OK; // Idempotent unlock
    }

    core->machine_state = ARGUS_STATE_UNLOCKED;

    esp_err_t err = ESP_OK;
    if (core->ops != NULL && core->ops->disable_driver != NULL) {
        err = core->ops->disable_driver();
    }

    return err;
}

esp_err_t argus_state_core_estop(argus_state_core_t *core)
{
    if (core == NULL) return ESP_ERR_INVALID_ARG;

    if (core->machine_state == ARGUS_STATE_EMERGENCY_STOPPED) {
        return ESP_OK; // Idempotent E-stop
    }

    // Preserve driver state retention destination
    if (core->machine_state == ARGUS_STATE_UNLOCKED) {
        core->estop_reset_destination = ARGUS_STATE_UNLOCKED;
    } else {
        core->estop_reset_destination = ARGUS_STATE_HOLDING;
    }

    core->estop_latched = true;
    core->command_generation++; // Increment generation counter to preempt in-flight commands
    core->machine_state = ARGUS_STATE_EMERGENCY_STOPPED;

    // Execute immediate pulse halt outside locks
    if (core->ops != NULL && core->ops->stop_immediate != NULL) {
        core->ops->stop_immediate();
    }

    return ESP_OK;
}

esp_err_t argus_state_core_reset_estop(argus_state_core_t *core)
{
    if (core == NULL) return ESP_ERR_INVALID_ARG;

    if (core->machine_state != ARGUS_STATE_EMERGENCY_STOPPED) {
        set_core_rejection_reason(core, "Cannot reset E-stop when not in EMERGENCY_STOPPED state");
        return ESP_ERR_INVALID_STATE;
    }

    core->estop_latched = false;
    core->machine_state = core->estop_reset_destination;
    return ESP_OK;
}

esp_err_t argus_state_core_recover(argus_state_core_t *core)
{
    if (core == NULL) return ESP_ERR_INVALID_ARG;

    if (core->machine_state == ARGUS_STATE_STARTING || core->machine_state == ARGUS_STATE_RUNNING || core->machine_state == ARGUS_STATE_DECELERATING) {
        set_core_rejection_reason(core, "Cannot recover while motion active");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t entry_gen = core->command_generation;
    core->machine_state = ARGUS_STATE_RECOVERING;

    esp_err_t rec_err = ESP_OK;
    if (core->ops != NULL && core->ops->recover != NULL) {
        rec_err = core->ops->recover();
    }

    // Check generation counter to ensure E-stop didn't occur during recovery
    if (core->command_generation != entry_gen || core->estop_latched || core->machine_state == ARGUS_STATE_EMERGENCY_STOPPED) {
        set_core_rejection_reason(core, "Recovery aborted: E-stop latched during recovery");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t err_val = 0;
    if (core->ops != NULL && core->ops->get_error != NULL) {
        err_val = core->ops->get_error();
    }

    if (rec_err == ESP_OK && err_val == 0) {
        core->fault_code = 0;
        core->machine_state = ARGUS_STATE_UNLOCKED;
    } else {
        core->machine_state = ARGUS_STATE_FAULTED;
        core->fault_code = (err_val != 0) ? err_val : rec_err;
        set_core_rejection_reason(core, "Recovery failed to clear lower-layer error");
    }

    return rec_err;
}

void argus_state_core_evaluate_periodic(argus_state_core_t *core, int32_t applied_rpm, int32_t generated_rpm, bool ramp_active, uint32_t latched_error)
{
    if (core == NULL) return;

    // Check for lower-layer errors
    if (latched_error != 0 && core->machine_state != ARGUS_STATE_FAULTED && core->machine_state != ARGUS_STATE_RECOVERING) {
        core->fault_code = latched_error;
        core->machine_state = ARGUS_STATE_FAULTED;
        ESP_LOGE(TAG, "State transition -> FAULTED due to lower layer error (%lu)", (unsigned long)latched_error);
    }

    // Transition: STARTING -> RUNNING when rate reaches configured target and ramp settles
    if (core->machine_state == ARGUS_STATE_STARTING) {
        if (applied_rpm == core->configured_target_rpm_milli &&
            generated_rpm == core->configured_target_rpm_milli &&
            !ramp_active) {
            core->machine_state = ARGUS_STATE_RUNNING;
            ESP_LOGI(TAG, "State transition: STARTING -> RUNNING (target=%ld mRPM reached)", (long)core->configured_target_rpm_milli);
        }
    }

    // Transition: DECELERATING -> HOLDING when applied and generated rates drop to zero
    if (core->machine_state == ARGUS_STATE_DECELERATING) {
        if (applied_rpm == 0 && generated_rpm == 0) {
            core->machine_state = ARGUS_STATE_HOLDING;
            ESP_LOGI(TAG, "State transition: DECELERATING -> HOLDING (deceleration complete)");
        }
    }
}

void argus_state_core_get_snapshot(const argus_state_core_t *core, argus_state_snapshot_t *snapshot)
{
    if (core == NULL || snapshot == NULL) return;

    snapshot->machine_state = core->machine_state;
    snapshot->configured_target_rpm_milli = core->configured_target_rpm_milli;
    snapshot->trajectory_target_rpm_milli = core->trajectory_target_rpm_milli;
    snapshot->applied_rpm_milli = core->applied_rpm_milli;
    snapshot->generated_rpm_milli = core->generated_rpm_milli;
    snapshot->generated_step_count = core->generated_step_count;
    snapshot->requested_forward = core->requested_forward;
    snapshot->applied_forward = core->applied_forward;
    snapshot->driver_enabled = core->driver_enabled;
    snapshot->ramp_active = core->ramp_active;
    snapshot->estop_latched = core->estop_latched;
    snapshot->fault_code = core->fault_code;
    snapshot->command_generation = core->command_generation;
    snapshot->feedback_available = false; // Open-loop architecture
    snprintf(snapshot->last_rejection_reason, sizeof(snapshot->last_rejection_reason), "%s", core->last_rejection_reason);
}

// ================= LIVE PRODUCTION STATE MANAGER WRAPPER =================

static void argus_state_mgr_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(20);

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        if (xSemaphoreTake(s_command_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }

        // Read lower layer snapshots without holding s_state_mutex
        argus_trajectory_snapshot_t traj_snap;
        argus_trajectory_get_snapshot(&traj_snap);

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        argus_state_core_evaluate_periodic(&s_prod_core, traj_snap.applied_rpm_milli, traj_snap.generated_rpm_milli, traj_snap.ramp_active, traj_snap.latched_error);
        xSemaphoreGive(s_state_mutex);

        xSemaphoreGive(s_command_mutex);
    }
}

esp_err_t argus_state_mgr_init(const argus_config_t *cfg, const argus_motion_ops_t *ops)
{
    if (s_command_mutex == NULL) {
        s_command_mutex = xSemaphoreCreateMutex();
        if (s_command_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    argus_state_core_init(&s_prod_core, cfg, ops);
    xSemaphoreGive(s_state_mutex);

    if (s_evaluator_task_handle == NULL) {
        xTaskCreate(argus_state_mgr_task, "state_mgr_task", 4096, NULL, 5, &s_evaluator_task_handle);
    }

    return ESP_OK;
}

esp_err_t argus_state_mgr_set_target(int32_t rpm_milli, bool forward)
{
    if (xSemaphoreTake(s_command_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    esp_err_t err = argus_state_core_set_target(&s_prod_core, rpm_milli, forward);

    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_command_mutex);
    return err;
}

esp_err_t argus_state_mgr_start(void)
{
    if (xSemaphoreTake(s_command_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    esp_err_t err = argus_state_core_start(&s_prod_core);

    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_command_mutex);
    return err;
}

esp_err_t argus_state_mgr_stop_normal(void)
{
    if (xSemaphoreTake(s_command_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    esp_err_t err = argus_state_core_stop_normal(&s_prod_core);

    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_command_mutex);
    return err;
}

esp_err_t argus_state_mgr_unlock(void)
{
    if (xSemaphoreTake(s_command_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    esp_err_t err = argus_state_core_unlock(&s_prod_core);

    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_command_mutex);
    return err;
}

esp_err_t argus_state_mgr_estop(void)
{
    // E-Stop bypasses s_command_mutex but blocks on s_state_mutex (non-preemptive; Phase 4B)
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    esp_err_t err = argus_state_core_estop(&s_prod_core);
    xSemaphoreGive(s_state_mutex);
    return err;
}

esp_err_t argus_state_mgr_reset_estop(void)
{
    if (xSemaphoreTake(s_command_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    esp_err_t err = argus_state_core_reset_estop(&s_prod_core);

    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_command_mutex);
    return err;
}

esp_err_t argus_state_mgr_recover(void)
{
    if (xSemaphoreTake(s_command_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    esp_err_t err = argus_state_core_recover(&s_prod_core);

    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_command_mutex);
    return err;
}

void argus_state_mgr_get_snapshot(argus_state_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;

    argus_trajectory_snapshot_t traj_snap;
    argus_trajectory_get_snapshot(&traj_snap);

    argus_step_gen_snapshot_t step_snap;
    argus_step_gen_get_snapshot(&step_snap);

    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }

    argus_state_core_get_snapshot(&s_prod_core, snapshot);

    // Populate dynamic lower-layer snapshot fields into returned snapshot
    snapshot->trajectory_target_rpm_milli = traj_snap.target_rpm_milli;
    snapshot->applied_rpm_milli = traj_snap.applied_rpm_milli;
    snapshot->generated_rpm_milli = step_snap.generated_rpm_milli;
    snapshot->generated_step_count = step_snap.generated_step_count;
    snapshot->applied_forward = traj_snap.applied_forward;
    snapshot->driver_enabled = step_snap.driver_enabled;
    snapshot->ramp_active = traj_snap.ramp_active;
    if (snapshot->fault_code == 0 && traj_snap.latched_error != 0) {
        snapshot->fault_code = traj_snap.latched_error;
    }

    if (s_state_mutex != NULL) {
        xSemaphoreGive(s_state_mutex);
    }
}
