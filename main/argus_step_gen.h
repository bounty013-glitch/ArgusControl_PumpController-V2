#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "argus_config.h"

// Backend error states
typedef enum {
    ARGUS_STEP_GEN_ERROR_NONE = 0,
    ARGUS_STEP_GEN_ERROR_INIT_FAILED,
    ARGUS_STEP_GEN_ERROR_TIMING_UPDATE_FAILED,
    ARGUS_STEP_GEN_ERROR_UNVERIFIED_ENABLE,
} argus_step_gen_error_t;

/**
 * @brief Initialize the step generator module.
 *        GPIO 5 is configured for active-low ENABLE output.
 *        To prevent glitches, GPIO 5 is preloaded to HIGH before configuring as output.
 * @param cfg Pointer to active configuration.
 * @return ESP_OK on success.
 */
esp_err_t argus_step_gen_init(const argus_config_t *cfg);

/**
 * @brief Arm the step generator while motion remains inhibited.
 *        Does NOT automatically enable the driver (GPIO 5 remains HIGH/disabled).
 * @return ESP_OK on success.
 */
esp_err_t argus_step_gen_arm(void);

/**
 * @brief Enable the motor driver (drive GPIO 5 LOW).
 *        Wait for the required 20 ms enable-settle delay.
 *        This function is idempotent.
 * @return ESP_OK on success.
 */
esp_err_t argus_step_gen_enable_driver(void);

/**
 * @brief Disable the motor driver (drive GPIO 5 HIGH).
 *        Will stop stepping safely first if actively running.
 *        This function is idempotent.
 * @return ESP_OK on success.
 */
esp_err_t argus_step_gen_disable_driver(void);

/**
 * @brief Query the active driver enablement state.
 * @return true if driver is enabled (GPIO 5 is LOW).
 */
bool argus_step_gen_is_driver_enabled(void);

/**
 * @brief Set output direction.
 *        Will be rejected if actively stepping (non-zero rate).
 *        Enforces UIM DIR hold and setup timing parameters physically.
 * @param forward true for forward (DIR HIGH), false for reverse (DIR LOW).
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if active.
 */
esp_err_t argus_step_gen_set_direction(bool forward);

/**
 * @brief Set the target rate exactly using milli-RPM and steps per revolution.
 *        Enforces minimum period requirements (6 us high + 6 us min low).
 *        Automatically enables the driver and waits 20 ms if not already enabled.
 * @param rpm_milli Target speed in milli-RPM.
 * @param steps_per_rev Target output command steps per shaft revolution.
 * @return ESP_OK on success, error code if update fails.
 */
esp_err_t argus_step_gen_set_rate_rpm_milli(int32_t rpm_milli, uint32_t steps_per_rev);

/**
 * @brief Start step pulse generation.
 * @return ESP_OK on success.
 */
esp_err_t argus_step_gen_start(void);

/**
 * @brief Stop step pulse generation immediately.
 *        Halts the timer counter outside critical sections, forces STEP low,
 *        keeps the driver enabled (GPIO 5 LOW) to retain holding torque.
 *        This call is idempotent.
 * @return ESP_OK on success.
 */
esp_err_t argus_step_gen_stop_immediate(void);

/**
 * @brief Request a stop, letting speed ramp to zero through the trajectory layer.
 * @return ESP_OK on success.
 */
esp_err_t argus_step_gen_stop_profiled(void);

/**
 * @brief Query the requested target speed.
 * @return Requested speed in milli-RPM (requested_rpm_milli).
 */
int32_t argus_step_gen_get_requested_rpm_milli(void);

/**
 * @brief Query the active generated speed.
 * @return Active speed in milli-RPM (generated_rpm_milli).
 */
int32_t argus_step_gen_get_generated_rpm_milli(void);

/**
 * @brief Read the signed, direction-aware step pulse count.
 * @return Count of pulses generated since last reset (generated_step_count).
 */
int64_t argus_step_gen_get_step_count(void);

/**
 * @brief Read the backend error state.
 * @return Current error code.
 */
argus_step_gen_error_t argus_step_gen_get_error(void);

/**
 * @brief Reset the step counter.
 */
void argus_step_gen_reset_step_count(void);

/**
 * @brief Explicitly clear step generator error state.
 */
void argus_step_gen_clear_error(void);

typedef struct {
    int32_t requested_rpm_milli;
    int32_t generated_rpm_milli;
    int64_t generated_step_count;
    bool driver_enabled;
    bool is_running;
    bool is_forward;
    argus_step_gen_error_t error_state;
} argus_step_gen_snapshot_t;

/**
 * @brief Retrieve a coherent snapshot of step generator state variables.
 */
void argus_step_gen_get_snapshot(argus_step_gen_snapshot_t *snapshot);
