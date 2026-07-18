#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Feedback provider interface structure.
 *        Allows clean future integration of physical encoder feedback.
 */
typedef struct {
    /**
     * @brief Check if physical feedback is currently active and available.
     * @return true if feedback is actively reading, false otherwise.
     */
    bool (*is_available)(void);

    /**
     * @brief Retrieve actual measured output speed in milli-RPM.
     *        If unavailable, returns 0 and error code.
     * @param out_rpm Pointer to store measured RPM.
     * @return ESP_OK if reading is valid, ESP_ERR_NOT_SUPPORTED if feedback is disabled.
     */
    esp_err_t (*get_actual_rpm)(int32_t *out_rpm);

    /**
     * @brief Retrieve actual physical shaft position in steps.
     * @param out_steps Pointer to store steps count.
     * @return ESP_OK if valid, error code otherwise.
     */
    esp_err_t (*get_actual_position)(int64_t *out_steps);
} argus_feedback_interface_t;

/**
 * @brief Retrieve the active feedback seam interface.
 *        By default, this interface is disabled and returns errors/zero values.
 */
const argus_feedback_interface_t *argus_feedback_get_interface(void);
