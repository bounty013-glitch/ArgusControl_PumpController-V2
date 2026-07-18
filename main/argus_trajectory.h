#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "argus_config.h"

/**
 * @brief Initialize the linear trajectory ramp engine.
 * @param cfg Active system hardware configuration.
 * @return ESP_OK on success.
 */
esp_err_t argus_trajectory_init(const argus_config_t *cfg);

/**
 * @brief Start background 20 ms trajectory periodic task.
 * @return ESP_OK on success.
 */
esp_err_t argus_trajectory_task_start(void);

/**
 * @brief Set target speed and direction.
 *        Trajectory ramp will smoothly transition applied speed to target rate.
 * @param rpm_milli Target speed in milli-RPM (e.g. 500 = 0.5 RPM).
 * @param forward true for forward direction, false for reverse.
 * @return ESP_OK on success.
 */
esp_err_t argus_trajectory_set_target_rpm_milli(int32_t rpm_milli, bool forward);

/**
 * @brief Normal stop: ramps applied speed down to zero at configured decel rate,
 *        then halts STEP pulses while retaining holding torque (ENA LOW).
 * @return ESP_OK on success.
 */
esp_err_t argus_trajectory_stop_normal(void);

/**
 * @brief Immediate stop: sets target and applied speed to zero immediately,
 *        halts STEP pulses instantly while retaining holding torque (ENA LOW).
 * @return ESP_OK on success.
 */
esp_err_t argus_trajectory_stop_immediate(void);

/**
 * @brief Unlock driver: halts STEP pulses immediately, sets target/applied speed to zero,
 *        drives ENA HIGH (releases motor holding torque).
 * @return ESP_OK on success.
 */
esp_err_t argus_trajectory_unlock(void);

/**
 * @brief Diagnostic recovery ('r'): halts STEP pulses, sets target/applied/generated to 0,
 *        drives ENA HIGH, waits 500 ms recovery interval, leaves driver unlocked.
 * @return ESP_OK on success.
 */
esp_err_t argus_trajectory_recover(void);

/**
 * @brief Explicitly clear trajectory latched errors and lower-layer errors.
 */
void argus_trajectory_clear_error(void);

/**
 * @brief Query current supervisory target speed.
 * @return Target speed in milli-RPM.
 */
int32_t argus_trajectory_get_target_rpm_milli(void);

/**
 * @brief Query current ramp-limited applied speed.
 * @return Applied speed in milli-RPM.
 */
int32_t argus_trajectory_get_applied_rpm_milli(void);

/**
 * @brief Query if trajectory ramp is currently active (applied != target).
 * @return true if ramping, false if settled.
 */
bool argus_trajectory_is_ramp_active(void);

/**
 * @brief Query if a direction reversal ramp through zero is in progress.
 * @return true if reversing.
 */
bool argus_trajectory_is_reversing(void);

/**
 * @brief Enable the motor driver through trajectory layer.
 * @return ESP_OK on success.
 */
esp_err_t argus_trajectory_enable_driver(void);

/**
 * @brief Execute a single 20 ms trajectory tick (used by task and unit tests).
 */
void argus_trajectory_step_tick_20ms(void);

typedef struct {
    int32_t target_rpm_milli;
    int32_t applied_rpm_milli;
    int32_t generated_rpm_milli;
    bool target_forward;
    bool applied_forward;
    bool ramp_active;
    bool is_reversing;
    uint32_t latched_error;
} argus_trajectory_snapshot_t;

/**
 * @brief Retrieve a coherent snapshot of trajectory engine variables and latched errors.
 */
void argus_trajectory_get_snapshot(argus_trajectory_snapshot_t *snapshot);
