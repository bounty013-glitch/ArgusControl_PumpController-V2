/** @file argus_tests_4b6.h */
#ifndef ARGUS_TESTS_4B6_H
#define ARGUS_TESTS_4B6_H

#include "esp_err.h"

esp_err_t test_4b6_factory_reset_decoder_acceptance(void);
esp_err_t test_4b6_factory_reset_decoder_rejections(void);
esp_err_t test_4b6_factory_reset_content_type_contract(void);
esp_err_t test_4b6_factory_reset_receive_contract(void);
esp_err_t test_4b6_factory_reset_receive_close_contract(void);
esp_err_t test_4b6_factory_reset_policy_matrix(void);
esp_err_t test_4b6_factory_reset_orchestration_success(void);
esp_err_t test_4b6_factory_reset_orchestration_revalidation(void);
esp_err_t test_4b6_factory_reset_orchestration_failures(void);
esp_err_t test_4b6_factory_reset_http_and_ui_contract(void);

#endif /* ARGUS_TESTS_4B6_H */
