#ifndef ARGUS_TESTS_4D3A_H
#define ARGUS_TESTS_4D3A_H

#include "esp_err.h"

esp_err_t test_4d3a_audit_mutation_lifecycle(void);
esp_err_t test_4d3a_audit_pagination(void);
esp_err_t test_4d3a_audit_query_strictness(void);
esp_err_t test_4d3a_transition_response_order(void);
esp_err_t test_4d3a_complete_route_inventory(void);
esp_err_t test_4d3a_browser_pagination_contract(void);

#endif
