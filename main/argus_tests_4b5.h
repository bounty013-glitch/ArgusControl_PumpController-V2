/**
 * @file argus_tests_4b5.h
 * @brief Phase 4B.5 controls-page and live-status tests.
 */

#ifndef ARGUS_TESTS_4B5_H
#define ARGUS_TESTS_4B5_H

#include "esp_err.h"

esp_err_t test_4b5_machine_status_json_contract(void);
esp_err_t test_4b5_controls_route_and_navigation_contract(void);
esp_err_t test_4b5_controls_command_contract(void);
esp_err_t test_4b5_controls_live_status_contract(void);

#endif /* ARGUS_TESTS_4B5_H */
