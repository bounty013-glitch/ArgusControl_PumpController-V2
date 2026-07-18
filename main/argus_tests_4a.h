/**
 * @file argus_tests_4a.h
 * @brief Phase 4A Pure Non-Motion Unit Test Suite Header
 */

#ifndef ARGUS_TESTS_4A_H
#define ARGUS_TESTS_4A_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run all Phase 4A pure non-motion unit tests.
 * @return ESP_OK if all 42 tests pass, ESP_FAIL if any test fails.
 */
esp_err_t argus_tests_4a_run_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_TESTS_4A_H */
