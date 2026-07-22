/**
 * @file argus_tests_4b4_step2.h
 * @brief Phase 4B.4 Step 2 browser-command endpoint tests.
 */

#ifndef ARGUS_TESTS_4B4_STEP2_H
#define ARGUS_TESTS_4B4_STEP2_H

#include "esp_err.h"

esp_err_t test_4b4_endpoint_registration_contract(void);
esp_err_t test_4b4_endpoint_body_receive_success(void);
esp_err_t test_4b4_endpoint_body_receive_failures(void);
esp_err_t test_4b4_endpoint_auth_and_decoder_rejections(void);
esp_err_t test_4b4_endpoint_admission_matrix(void);
esp_err_t test_4b4_endpoint_argument_free_envelopes(void);
esp_err_t test_4b4_endpoint_set_target_envelope(void);
esp_err_t test_4b4_endpoint_generation_capture_order(void);
esp_err_t test_4b4_endpoint_dispatch_result_mapping(void);
esp_err_t test_4b4_endpoint_response_contract(void);
esp_err_t test_4b4_endpoint_routing_field_rejections(void);
esp_err_t test_4b4_endpoint_invalid_ops_are_isolated(void);

#endif /* ARGUS_TESTS_4B4_STEP2_H */
