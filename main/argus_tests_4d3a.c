#include "argus_tests_4d3a.h"

#include <string.h>

#include "argus_http_route_inventory.h"
#include "argus_http_server.h"
#include "argus_security_audit.h"
#include "argus_security_http.h"
#include "argus_security_transition.h"

#define CHECK(condition) do { if (!(condition)) return ESP_FAIL; } while (0)

esp_err_t test_4d3a_audit_mutation_lifecycle(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    argus_security_audit_test_clear_finalization_degraded();
    argus_security_audit_mutation_t success;
    CHECK(argus_security_audit_mutation_begin(
              ARGUS_AUDIT_ACCOUNT_CHANGED, ARGUS_PRINCIPAL_HUMAN,
              "test_actor", "test_target", "TEST", "enable", 7U,
              &success) == ESP_OK);
    CHECK(success.lifecycle_id != 0U);
    uint16_t success_id = success.lifecycle_id;
    CHECK(argus_security_audit_mutation_finish(&success, true) == ESP_OK);
    memset(&success, 0, sizeof(success));

    argus_security_audit_mutation_t failure;
    CHECK(argus_security_audit_mutation_begin(
              ARGUS_AUDIT_ROLE_CHANGED, ARGUS_PRINCIPAL_HUMAN,
              "test_actor", "test_role", "TEST", "delete", 7U,
              &failure) == ESP_OK);
    CHECK(failure.lifecycle_id != 0U);
    uint16_t failure_id = failure.lifecycle_id;
    CHECK(argus_security_audit_mutation_finish(&failure, false) == ESP_OK);
    memset(&failure, 0, sizeof(failure));

    argus_security_audit_page_t page;
    CHECK(argus_security_audit_read_page(0U, 4U, &page) == ESP_OK);
    CHECK(page.count == 4U);
    CHECK(page.records[0].outcome == ARGUS_AUDIT_OUTCOME_FAILED);
    CHECK(page.records[0].lifecycle_id == failure_id);
    CHECK(page.records[1].outcome == ARGUS_AUDIT_OUTCOME_PREPARED);
    CHECK(page.records[1].lifecycle_id == failure_id);
    CHECK(page.records[2].outcome == ARGUS_AUDIT_OUTCOME_SUCCESS);
    CHECK(page.records[2].lifecycle_id == success_id);
    CHECK(page.records[3].outcome == ARGUS_AUDIT_OUTCOME_PREPARED);
    CHECK(page.records[3].lifecycle_id == success_id);
    CHECK(strstr(page.records[3].reason, "prepared") != NULL);
    CHECK(strstr(page.records[2].reason, "succeeded") != NULL);
    CHECK(strstr(page.records[0].reason, "failed") != NULL);
    CHECK(strstr(page.records[0].actor, "password") == NULL);
    memset(&page, 0, sizeof(page));

    bool mutation_called = false;
    argus_security_audit_test_fail_next_required();
    argus_security_audit_mutation_t blocked;
    CHECK(argus_security_audit_mutation_begin(
              ARGUS_AUDIT_ACCOUNT_CHANGED, ARGUS_PRINCIPAL_HUMAN,
              "test_actor", "blocked", "TEST", "disable", 7U,
              &blocked) != ESP_OK);
    CHECK(!mutation_called);

    CHECK(argus_security_audit_mutation_begin(
              ARGUS_AUDIT_ACCOUNT_CHANGED, ARGUS_PRINCIPAL_HUMAN,
              "test_actor", "indeterminate", "TEST", "update", 7U,
              &blocked) == ESP_OK);
    mutation_called = true;
    argus_security_audit_test_fail_next_required();
    CHECK(argus_security_audit_mutation_finish(&blocked, true) != ESP_OK);
    CHECK(mutation_called);
    argus_security_audit_status_t status;
    CHECK(argus_security_audit_get_status(&status) == ESP_OK);
    CHECK(status.finalization_degraded);
    memset(&blocked, 0, sizeof(blocked));
    CHECK(argus_security_audit_mutation_begin(
              ARGUS_AUDIT_ACCOUNT_CHANGED, ARGUS_PRINCIPAL_HUMAN,
              "test_actor", "later", "TEST", "enable", 7U,
              &blocked) == ESP_ERR_INVALID_STATE);
    argus_security_audit_test_clear_finalization_degraded();
#endif
    return ESP_OK;
}

esp_err_t test_4d3a_audit_pagination(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    for (uint32_t i = 0U; i < 18U; ++i) {
        CHECK(argus_security_audit_append(
                  ARGUS_AUDIT_ADMIN_DENIED,
                  ARGUS_AUDIT_OUTCOME_REJECTED,
                  ARGUS_PRINCIPAL_HUMAN, "page_actor",
                  "page_target", "TEST", "pagination_seed",
                  i + 1U, true) == ESP_OK);
    }
    argus_security_audit_page_t first;
    argus_security_audit_page_t second;
    CHECK(argus_security_audit_read_page(0U, 16U, &first) == ESP_OK);
    CHECK(first.count == 16U && first.has_more);
    CHECK(first.next_before == first.records[15].sequence);
    CHECK(argus_security_audit_read_page(
              first.next_before, 16U, &second) == ESP_OK);
    CHECK(second.count > 0U);
    CHECK(second.records[0].sequence < first.records[15].sequence);
    for (uint32_t i = 1U; i < first.count; ++i) {
        CHECK(first.records[i - 1U].sequence >
              first.records[i].sequence);
    }
    for (uint32_t i = 0U; i < first.count; ++i) {
        for (uint32_t j = 0U; j < second.count; ++j) {
            CHECK(first.records[i].sequence != second.records[j].sequence);
        }
    }
    CHECK(argus_security_audit_read_page(
              UINT64_MAX, 16U, &second) == ESP_ERR_INVALID_ARG);
    CHECK(argus_security_audit_read_page(0U, 0U, &second) ==
          ESP_ERR_INVALID_ARG);
    CHECK(argus_security_audit_read_page(
              0U, ARGUS_AUDIT_PAGE_MAX + 1U, &second) ==
          ESP_ERR_INVALID_ARG);
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
#endif
    return ESP_OK;
}

esp_err_t test_4d3a_audit_query_strictness(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    argus_security_http_audit_query_t query;
    CHECK(argus_security_http_test_parse_audit_query(NULL, &query));
    CHECK(query.limit == 16U && query.before_sequence == 0U);
    CHECK(argus_security_http_test_parse_audit_query(
        "limit=4&before=123", &query));
    CHECK(query.limit == 4U && query.before_sequence == 123U);
    CHECK(argus_security_http_test_parse_audit_query(
        "before=123&limit=16", &query));
    CHECK(!argus_security_http_test_parse_audit_query("limit=0", &query));
    CHECK(!argus_security_http_test_parse_audit_query("limit=17", &query));
    CHECK(!argus_security_http_test_parse_audit_query(
        "limit=2&limit=3", &query));
    CHECK(!argus_security_http_test_parse_audit_query("before=0", &query));
    CHECK(!argus_security_http_test_parse_audit_query(
        "before=1&before=2", &query));
    CHECK(!argus_security_http_test_parse_audit_query(
        "before=18446744073709551616", &query));
    CHECK(!argus_security_http_test_parse_audit_query("offset=1", &query));
    CHECK(!argus_security_http_test_parse_audit_query("limit=1&", &query));
#endif
    return ESP_OK;
}

typedef struct {
    int sequence;
    int response_order;
    int arm_order;
    size_t response_calls;
    size_t arm_calls;
    esp_err_t response_result;
    esp_err_t arm_result;
} transition_trace_t;

static esp_err_t traced_response(void *ctx)
{
    transition_trace_t *trace = ctx;
    trace->response_calls++;
    trace->response_order = ++trace->sequence;
    return trace->response_result;
}

static esp_err_t traced_arm(void *ctx)
{
    transition_trace_t *trace = ctx;
    trace->arm_calls++;
    trace->arm_order = ++trace->sequence;
    return trace->arm_result;
}

esp_err_t test_4d3a_transition_response_order(void)
{
    argus_security_transition_t transition = {0};
    CHECK(argus_security_transition_prepare(&transition) ==
          ESP_ERR_INVALID_STATE);
    argus_security_transition_set_resource_ready(&transition, true);
    CHECK(argus_security_transition_prepare(&transition) == ESP_OK);
    CHECK(argus_security_transition_prepare(&transition) ==
          ESP_ERR_NOT_FINISHED);
    transition_trace_t trace = {
        .response_result = ESP_OK,
        .arm_result = ESP_OK,
    };
    CHECK(argus_security_transition_respond_then_arm(
              &transition, traced_response, traced_arm, &trace) == ESP_OK);
    CHECK(trace.response_order == 1 && trace.arm_order == 2);
    CHECK(trace.response_calls == 1U && trace.arm_calls == 1U);
    CHECK(argus_security_transition_claim_callback(&transition));
    CHECK(!argus_security_transition_claim_callback(&transition));

    memset(&trace, 0, sizeof(trace));
    trace.response_result = ESP_FAIL;
    trace.arm_result = ESP_OK;
    CHECK(argus_security_transition_prepare(&transition) == ESP_OK);
    CHECK(argus_security_transition_respond_then_arm(
              &transition, traced_response, traced_arm, &trace) ==
          ESP_FAIL);
    CHECK(trace.response_calls == 1U && trace.arm_calls == 0U);
    CHECK(!argus_security_transition_claim_callback(&transition));

    memset(&trace, 0, sizeof(trace));
    trace.response_result = ESP_OK;
    trace.arm_result = ESP_ERR_NO_MEM;
    CHECK(argus_security_transition_prepare(&transition) == ESP_OK);
    CHECK(argus_security_transition_respond_then_arm(
              &transition, traced_response, traced_arm, &trace) ==
          ESP_ERR_NO_MEM);
    CHECK(trace.response_order == 1 && trace.arm_order == 2);
    CHECK(!argus_security_transition_claim_callback(&transition));
    return ESP_OK;
}

static bool inventory_contains(
    const argus_http_route_inventory_entry_t *inventory,
    size_t count, const char *path, httpd_method_t method,
    bool registered)
{
    for (size_t i = 0U; i < count; ++i) {
        if (inventory[i].method == method &&
            strcmp(inventory[i].path, path) == 0) {
            return inventory[i].registered == registered;
        }
    }
    return false;
}

static size_t registered_occurrences(
    const char *path, httpd_method_t method)
{
    size_t occurrences = 0U;
    size_t main_count = argus_http_test_registered_route_count();
    for (size_t i = 0U; i < main_count; ++i) {
        const char *registered_path = NULL;
        httpd_method_t registered_method;
        if (argus_http_test_registered_route(
                i, &registered_path, &registered_method) &&
            registered_method == method &&
            strcmp(registered_path, path) == 0) {
            occurrences++;
        }
    }
    size_t security_count = argus_security_http_test_route_count();
    for (size_t i = 0U; i < security_count; ++i) {
        const char *registered_path = NULL;
        httpd_method_t registered_method;
        if (argus_security_http_test_registered_route(
                i, &registered_path, &registered_method) &&
            registered_method == method &&
            strcmp(registered_path, path) == 0) {
            occurrences++;
        }
    }
    return occurrences;
}

esp_err_t test_4d3a_complete_route_inventory(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    size_t inventory_count = 0U;
    const argus_http_route_inventory_entry_t *inventory =
        argus_http_route_inventory(&inventory_count);
    CHECK(inventory != NULL && inventory_count == 35U);
    CHECK(argus_http_route_inventory_validate());
    size_t active = 0U;
    for (size_t i = 0U; i < inventory_count; ++i) {
        active += inventory[i].registered ? 1U : 0U;
    }
    size_t main_count = argus_http_test_registered_route_count();
    size_t security_count = argus_security_http_test_route_count();
    CHECK(active == main_count + security_count);
    for (size_t i = 0U; i < inventory_count; ++i) {
        CHECK(registered_occurrences(
                  inventory[i].path, inventory[i].method) ==
              (inventory[i].registered ? 1U : 0U));
    }
    for (size_t i = 0U; i < main_count; ++i) {
        const char *path = NULL;
        httpd_method_t method;
        CHECK(argus_http_test_registered_route(i, &path, &method));
        CHECK(inventory_contains(
            inventory, inventory_count, path, method, true));
    }
    for (size_t i = 0U; i < security_count; ++i) {
        const char *path = NULL;
        httpd_method_t method;
        CHECK(argus_security_http_test_registered_route(
            i, &path, &method));
        CHECK(inventory_contains(
            inventory, inventory_count, path, method, true));
    }
    CHECK(inventory_contains(
        inventory, inventory_count, "/api/logout", HTTP_GET, false));
    CHECK(inventory_contains(
        inventory, inventory_count, "/api/auth/reauth", HTTP_POST, true));
    CHECK(inventory_contains(
        inventory, inventory_count,
        "/api/auth/change-password", HTTP_POST, true));
#endif
    return ESP_OK;
}

esp_err_t test_4d3a_browser_pagination_contract(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    size_t length = 0U;
    const char *page = argus_http_test_commission_page(&length);
    CHECK(page != NULL && length > 0U);
    CHECK(strstr(page, "/api/security/audit") != NULL);
    CHECK(strstr(page, "Authorization: Basic") == NULL);
#endif
    return ESP_OK;
}
