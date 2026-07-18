#pragma once

#include "esp_err.h"

/**
 * @brief Run pure non-motion unit tests (mock operations table, 0 hardware touch).
 * @return ESP_OK if all pure tests pass.
 */
esp_err_t argus_tests_run_all(void);

/**
 * @brief Run interactive hardware acceptance test suite (DANGER — HARDWARE MOTION).
 * @return ESP_OK if all hardware tests complete.
 */
esp_err_t argus_tests_run_hardware_acceptance(void);
