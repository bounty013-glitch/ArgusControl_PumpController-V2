#ifndef ARGUS_TESTS_4D3_H
#define ARGUS_TESTS_4D3_H

#include "esp_err.h"

esp_err_t test_4d3_username_policy(void);
esp_err_t test_4d3_password_policy(void);
esp_err_t test_4d3_authorization_role_matrix(void);
esp_err_t test_4d3_authorization_capability_denials(void);
esp_err_t test_4d3_authorization_target_ceilings(void);
esp_err_t test_4d3_authorization_delegation_ceiling(void);
esp_err_t test_4d3_session_issue_authenticate_and_csrf(void);
esp_err_t test_4d3_session_population_limits(void);
esp_err_t test_4d3_session_expiry_boundaries(void);
esp_err_t test_4d3_session_collision_retry(void);
esp_err_t test_4d3_session_revocation(void);
esp_err_t test_4d3_session_malformed_tokens(void);
esp_err_t test_4d3_directory_role_integrity(void);
esp_err_t test_4d3_login_decoder_strictness(void);
esp_err_t test_4d3_command_capability_mapping(void);
esp_err_t test_4d3_browser_artifact_contract(void);
esp_err_t test_4d3_security_route_inventory(void);

#endif
