#pragma once

#include "esp_err.h"

/**
 * @brief Run all automated unit tests for configuration, conversions, 
 *        and phase accumulator math.
 * @return ESP_OK if all tests pass, ESP_FAIL otherwise.
 */
esp_err_t argus_tests_run_all(void);
