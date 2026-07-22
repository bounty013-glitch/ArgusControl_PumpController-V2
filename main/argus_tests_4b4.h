/**
 * @file argus_tests_4b4.h
 * @brief Phase 4B.4 pure browser-command decoder tests.
 */

#ifndef ARGUS_TESTS_4B4_H
#define ARGUS_TESTS_4B4_H

#include "esp_err.h"

esp_err_t test_4b4_decode_argument_free_commands(void);
esp_err_t test_4b4_decode_set_target_success(void);
esp_err_t test_4b4_decode_malformed_and_top_level(void);
esp_err_t test_4b4_decode_command_rejections(void);
esp_err_t test_4b4_decode_field_contract_rejections(void);
esp_err_t test_4b4_decode_target_rejections(void);
esp_err_t test_4b4_decode_forward_rejections(void);
esp_err_t test_4b4_decode_length_trailing_and_nul(void);
esp_err_t test_4b4_decode_output_reuse_and_routing_rejections(void);

#endif /* ARGUS_TESTS_4B4_H */
