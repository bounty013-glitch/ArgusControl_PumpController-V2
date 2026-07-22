/**
 * @file argus_tests_4b5.c
 * @brief Pure contracts for the Phase 4B.5 embedded controls page.
 */

#include "argus_tests_4b5.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "argus_http_server.h"
#include "argus_state_mgr.h"

#define TEST_CHECK(cond) do { if (!(cond)) return ESP_FAIL; } while (0)

static bool bounded_contains(const char *data, size_t data_len, const char *needle)
{
    if (data == NULL || needle == NULL) return false;
    size_t needle_len = strlen(needle);
    if (needle_len == 0U || needle_len > data_len) return false;
    for (size_t i = 0; i <= data_len - needle_len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static size_t bounded_count(const char *data, size_t data_len, const char *needle)
{
    size_t count = 0U;
    size_t needle_len = needle == NULL ? 0U : strlen(needle);
    if (data == NULL || needle_len == 0U || needle_len > data_len) return 0U;
    for (size_t i = 0; i <= data_len - needle_len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) {
            count++;
            i += needle_len - 1U;
        }
    }
    return count;
}

esp_err_t test_4b5_machine_status_json_contract(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    argus_state_snapshot_t snapshot = {
        .machine_state = ARGUS_STATE_RUNNING,
        .configured_target_rpm_milli = 8000,
        .applied_rpm_milli = 7900,
        .generated_rpm_milli = 8000,
        .requested_forward = true,
        .applied_forward = false,
        .driver_enabled = true,
        .ramp_active = true,
        .estop_latched = false,
        .fault_code = 17U,
        .command_generation = 42U,
        .feedback_available = false,
    };
    strlcpy(snapshot.last_rejection_reason, "bad \"field\"\\path\nnext",
            sizeof(snapshot.last_rejection_reason));
    argus_state_snapshot_t before = snapshot;

    char json[512];
    int len = argus_http_test_format_machine_status_json(&snapshot, json,
                                                          sizeof(json));
    TEST_CHECK(len > 0 && (size_t)len == strlen(json));
    TEST_CHECK(memcmp(&snapshot, &before, sizeof(snapshot)) == 0);
    TEST_CHECK(strstr(json, "\"state\":\"RUNNING\"") != NULL);
    TEST_CHECK(strstr(json, "\"target_rpm_milli\":8000") != NULL);
    TEST_CHECK(strstr(json, "\"applied_rpm_milli\":7900") != NULL);
    TEST_CHECK(strstr(json, "\"generated_rpm_milli\":8000") != NULL);
    TEST_CHECK(strstr(json, "\"requested_forward\":true") != NULL);
    TEST_CHECK(strstr(json, "\"applied_forward\":false") != NULL);
    TEST_CHECK(strstr(json, "\"driver_enabled\":true") != NULL);
    TEST_CHECK(strstr(json, "\"estop_latched\":false") != NULL);
    TEST_CHECK(strstr(json, "\"fault_code\":17") != NULL);
    TEST_CHECK(strstr(json, "\"command_generation\":42") != NULL);
    TEST_CHECK(strstr(json, "\"feedback_available\":false") != NULL);
    TEST_CHECK(strstr(json, "bad \\\"field\\\"\\\\path next") != NULL);
    TEST_CHECK(strstr(json, "password") == NULL);
    TEST_CHECK(strstr(json, "token") == NULL);

    char too_small[32];
    TEST_CHECK(argus_http_test_format_machine_status_json(
                   &snapshot, too_small, sizeof(too_small)) < 0);
    TEST_CHECK(argus_http_test_format_machine_status_json(NULL, json,
                                                          sizeof(json)) < 0);
#endif
    return ESP_OK;
}

esp_err_t test_4b5_controls_route_and_navigation_contract(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    TEST_CHECK(argus_http_test_controls_registration());
    size_t page_len = 0U;
    const char *page = argus_http_test_controls_page(&page_len);
    TEST_CHECK(page != NULL && page_len > 4096U && page_len < 65536U);
    TEST_CHECK(bounded_contains(page, page_len, "Argus Motion Controls"));
    TEST_CHECK(bounded_contains(page, page_len, "href=\"/\""));
    TEST_CHECK(bounded_contains(page, page_len, "href=\"/api/logout\""));
    TEST_CHECK(bounded_contains(page, page_len, "credentials: \"same-origin\""));
    TEST_CHECK(bounded_contains(page, page_len, "cache: \"no-store\""));
    TEST_CHECK(!bounded_contains(page, page_len, "window.argusRaw"));
    TEST_CHECK(!bounded_contains(page, page_len, "Developer Tools"));
#endif
    return ESP_OK;
}

esp_err_t test_4b5_controls_command_contract(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    size_t page_len = 0U;
    const char *page = argus_http_test_controls_page(&page_len);
    TEST_CHECK(bounded_count(page, page_len, "fetch(\"/api/command\"") == 1U);
    TEST_CHECK(bounded_contains(page, page_len, "command: \"set_target\""));
    TEST_CHECK(bounded_contains(page, page_len, "target_rpm_milli: milli"));
    TEST_CHECK(bounded_contains(page, page_len, "forward: byId(\"dir-forward\").checked"));
    TEST_CHECK(bounded_contains(page, page_len, "{ command: \"start\" }"));
    TEST_CHECK(bounded_contains(page, page_len, "{ command: \"stop\" }"));
    TEST_CHECK(bounded_contains(page, page_len, "{ command: \"unlock\" }"));
    TEST_CHECK(bounded_contains(page, page_len, "{ command: \"estop\" }"));
    TEST_CHECK(bounded_contains(page, page_len, "{ command: \"reset_estop\" }"));
    TEST_CHECK(bounded_contains(page, page_len, "{ command: \"recover\" }"));
    TEST_CHECK(!bounded_contains(page, page_len, "authority_generation:"));
    TEST_CHECK(!bounded_contains(page, page_len, "authority_owner:"));
    TEST_CHECK(!bounded_contains(page, page_len, "source:"));
    TEST_CHECK(bounded_contains(page, page_len, "runtime.ordinaryCommandPending"));
    TEST_CHECK(bounded_contains(page, page_len, "runtime.estopCommandPending"));
    TEST_CHECK(bounded_contains(page, page_len, "runtime.displayedCommandSequence"));
    TEST_CHECK(bounded_contains(page, page_len, "sequence !== runtime.displayedCommandSequence"));
    TEST_CHECK(bounded_contains(page, page_len, "rpmStringToMilli"));
    TEST_CHECK(bounded_contains(page, page_len, "milli >= 0 && milli <= 200000"));
    TEST_CHECK(bounded_contains(page, page_len, "Admission is not proof of physical motion"));
#endif
    return ESP_OK;
}

esp_err_t test_4b5_controls_live_status_contract(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    size_t page_len = 0U;
    const char *page = argus_http_test_controls_page(&page_len);
    TEST_CHECK(bounded_count(page, page_len, "fetch(\"/api/status\"") == 1U);
    TEST_CHECK(bounded_contains(page, page_len, "const POLL_MS = 750"));
    TEST_CHECK(bounded_contains(page, page_len, "const HIDDEN_POLL_MS = 5000"));
    TEST_CHECK(bounded_contains(page, page_len, "const REQUEST_TIMEOUT_MS = 2000"));
    TEST_CHECK(bounded_contains(page, page_len, "const STALE_MS = 3000"));
    TEST_CHECK(bounded_contains(page, page_len, "if (runtime.statusInFlight)"));
    TEST_CHECK(bounded_contains(page, page_len, "new AbortController()"));
    TEST_CHECK(bounded_contains(page, page_len, "visibilitychange"));
    TEST_CHECK(bounded_contains(page, page_len, "pagehide"));
    TEST_CHECK(bounded_contains(page, page_len, "runtime.reconcileRequested"));
    TEST_CHECK(bounded_contains(page, page_len, "Last known values are marked stale"));
    TEST_CHECK(bounded_contains(page, page_len, "runtime.unauthorized"));
    TEST_CHECK(bounded_contains(page, page_len, "validStatus(data)"));
    TEST_CHECK(bounded_contains(page, page_len, "m.state === \"FAULTED\""));
    TEST_CHECK(bounded_contains(page, page_len, "m.estop_latched"));
    TEST_CHECK(bounded_contains(page, page_len,
                                "byId(\"cmd-estop\").disabled = runtime.estopCommandPending || runtime.unauthorized || !runtime.pageActive"));
    TEST_CHECK(!bounded_contains(page, page_len,
                                 "byId(\"cmd-estop\").disabled = runtime.ordinaryCommandPending"));
    TEST_CHECK(!bounded_contains(page, page_len, "setInterval(requestStatus"));
#endif
    return ESP_OK;
}
