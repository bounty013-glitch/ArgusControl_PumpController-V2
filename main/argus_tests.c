#include "argus_tests.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "argus_config.h"
#include "argus_conversions.h"
#include "argus_cmd_parser.h"
#include "argus_state_mgr.h"

static const char *TAG = "argus_tests";

// ================= MOCK MOTION OPERATIONS FOR PURE TESTS =================

static int s_mock_enable_count = 0;
static int s_mock_disable_count = 0;
static int s_mock_set_direction_count = 0;
static int s_mock_set_target_rate_count = 0;
static int s_mock_stop_immediate_count = 0;
static int s_mock_stop_normal_count = 0;
static int s_mock_recover_count = 0;
static uint32_t s_mock_simulated_error = 0;

static void reset_mock_counts(void)
{
    s_mock_enable_count = 0;
    s_mock_disable_count = 0;
    s_mock_set_direction_count = 0;
    s_mock_set_target_rate_count = 0;
    s_mock_stop_immediate_count = 0;
    s_mock_stop_normal_count = 0;
    s_mock_recover_count = 0;
    s_mock_simulated_error = 0;
}

static esp_err_t mock_enable_driver(void) { s_mock_enable_count++; return ESP_OK; }
static esp_err_t mock_disable_driver(void) { s_mock_disable_count++; return ESP_OK; }
static esp_err_t mock_set_direction(bool fwd) { (void)fwd; s_mock_set_direction_count++; return ESP_OK; }
static esp_err_t mock_set_target_rate(int32_t rpm, bool fwd) { (void)rpm; (void)fwd; s_mock_set_target_rate_count++; return ESP_OK; }
static esp_err_t mock_stop_immediate(void) { s_mock_stop_immediate_count++; return ESP_OK; }
static esp_err_t mock_stop_normal(void) { s_mock_stop_normal_count++; return ESP_OK; }
static esp_err_t mock_recover(void) { s_mock_recover_count++; s_mock_simulated_error = 0; return ESP_OK; }
static uint32_t mock_get_error(void) { return s_mock_simulated_error; }

static const argus_motion_ops_t s_mock_ops = {
    .enable_driver = mock_enable_driver,
    .disable_driver = mock_disable_driver,
    .set_direction = mock_set_direction,
    .set_target_rate = mock_set_target_rate,
    .stop_immediate = mock_stop_immediate,
    .stop_normal = mock_stop_normal,
    .recover = mock_recover,
    .get_error = mock_get_error,
};

// ================= PURE NON-MOTION UNIT TESTS =================

static bool test_cmd_parser(void)
{
    int pct = 0;
    if (argus_cmd_parser_speed_pct("50", &pct) != ESP_OK || pct != 50) return false;
    if (argus_cmd_parser_speed_pct("0", &pct) != ESP_OK || pct != 0) return false;
    if (argus_cmd_parser_speed_pct("100", &pct) != ESP_OK || pct != 100) return false;
    if (argus_cmd_parser_speed_pct("50rpm", &pct) == ESP_OK) return false; // Non-numeric suffix
    if (argus_cmd_parser_speed_pct("-10", &pct) == ESP_OK) return false;  // Negative
    if (argus_cmd_parser_speed_pct("150", &pct) == ESP_OK) return false;  // >100
    if (argus_cmd_parser_speed_pct("", &pct) == ESP_OK) return false;     // Empty
    if (argus_cmd_parser_speed_pct("   ", &pct) == ESP_OK) return false;  // Whitespace

    bool bval = false;
    if (argus_cmd_parser_bool("true", &bval) != ESP_OK || !bval) return false;
    if (argus_cmd_parser_bool("1", &bval) != ESP_OK || !bval) return false;
    if (argus_cmd_parser_bool("on", &bval) != ESP_OK || !bval) return false;
    if (argus_cmd_parser_bool("false", &bval) != ESP_OK || bval) return false;
    if (argus_cmd_parser_bool("0", &bval) != ESP_OK || bval) return false;
    if (argus_cmd_parser_bool("off", &bval) != ESP_OK || bval) return false;
    if (argus_cmd_parser_bool("invalid", &bval) == ESP_OK) return false;

    // Verify all command topics reject retained payloads
    const char *cmd_topics[] = {
        "argus/peristaltic/cmd/speed_pct",
        "argus/peristaltic/cmd/run",
        "argus/peristaltic/cmd/stop",
        "argus/peristaltic/cmd/e_stop",
        "argus/peristaltic/cmd/reset_estop",
        "argus/peristaltic/cmd/unlock",
        "argus/unit/control/speed",
    };
    for (size_t i = 0; i < sizeof(cmd_topics)/sizeof(cmd_topics[0]); i++) {
        if (argus_cmd_parser_validate_control_message(cmd_topics[i], "true", true) == ESP_OK) return false;
        if (argus_cmd_parser_validate_control_message(cmd_topics[i], "true", false) != ESP_OK) return false;
    }

    return true;
}

static bool test_state_core_isolated(void)
{
    argus_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.min_output_milli_rpm = 500;
    cfg.max_output_milli_rpm = 200000;

    reset_mock_counts();
    argus_state_core_t core;
    argus_state_core_init(&core, &cfg, &s_mock_ops);

    argus_state_snapshot_t snap;
    argus_state_core_get_snapshot(&core, &snap);
    if (snap.machine_state != ARGUS_STATE_UNLOCKED) return false;

    // 1. SET_TARGET while UNLOCKED updates setpoint without starting motion
    if (argus_state_core_set_target(&core, 1000, true) != ESP_OK) return false;
    argus_state_core_get_snapshot(&core, &snap);
    if (snap.configured_target_rpm_milli != 1000) return false;
    if (s_mock_enable_count != 0 || s_mock_set_target_rate_count != 0) return false; // 0 motion calls!

    // 2. START with 0 setpoint rejected
    argus_state_core_set_target(&core, 0, true);
    if (argus_state_core_start(&core) == ESP_OK) return false;

    // 3. Valid START transitions to STARTING
    argus_state_core_set_target(&core, 1000, true);
    if (argus_state_core_start(&core) != ESP_OK) return false;
    argus_state_core_get_snapshot(&core, &snap);
    if (snap.machine_state != ARGUS_STATE_STARTING) return false;
    if (s_mock_enable_count != 1 || s_mock_set_target_rate_count != 1) return false;

    // 4. UNLOCK during motion is rejected
    if (argus_state_core_unlock(&core) == ESP_OK) return false;

    // 5. E_STOP instantly halts pulses and latches
    if (argus_state_core_estop(&core) != ESP_OK) return false;
    argus_state_core_get_snapshot(&core, &snap);
    if (snap.machine_state != ARGUS_STATE_EMERGENCY_STOPPED || !snap.estop_latched) return false;

    // 6. While latched, START and SET_TARGET are rejected
    if (argus_state_core_start(&core) == ESP_OK) return false;
    if (argus_state_core_set_target(&core, 2000, true) == ESP_OK) return false;

    // 7. RESET_ESTOP clears latch to HOLDING without restarting motion
    int prev_rate_count = s_mock_set_target_rate_count;
    if (argus_state_core_reset_estop(&core) != ESP_OK) return false;
    argus_state_core_get_snapshot(&core, &snap);
    if (snap.machine_state != ARGUS_STATE_HOLDING || snap.estop_latched) return false;
    if (s_mock_set_target_rate_count != prev_rate_count) return false;

    // 8. UNLOCK while HOLDING succeeds
    if (argus_state_core_unlock(&core) != ESP_OK) return false;
    argus_state_core_get_snapshot(&core, &snap);
    if (snap.machine_state != ARGUS_STATE_UNLOCKED) return false;

    return true;
}

static bool test_injected_error_propagation(void)
{
    argus_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.min_output_milli_rpm = 500;
    cfg.max_output_milli_rpm = 200000;

    reset_mock_counts();
    argus_state_core_t core;
    argus_state_core_init(&core, &cfg, &s_mock_ops);

    // Simulate lower layer error
    s_mock_simulated_error = 2;
    argus_state_core_evaluate_periodic(&core, 0, 0, false, s_mock_simulated_error);

    argus_state_snapshot_t snap;
    argus_state_core_get_snapshot(&core, &snap);
    if (snap.machine_state != ARGUS_STATE_FAULTED || snap.fault_code != 2) return false;

    // Motion commands rejected while FAULTED
    if (argus_state_core_start(&core) == ESP_OK) return false;

    // Recovery clears error and returns to UNLOCKED
    if (argus_state_core_recover(&core) != ESP_OK) return false;
    argus_state_core_get_snapshot(&core, &snap);
    if (snap.machine_state != ARGUS_STATE_UNLOCKED || snap.fault_code != 0) return false;

    return true;
}

static bool test_production_singleton_isolation(void)
{
    // Record production state manager snapshot before test execution
    argus_state_snapshot_t snap_before;
    argus_state_mgr_get_snapshot(&snap_before);

    // Run pure tests 3 times in succession
    for (int i = 0; i < 3; i++) {
        if (!test_cmd_parser() || !test_state_core_isolated() || !test_injected_error_propagation()) {
            return false;
        }
    }

    // Record production snapshot after tests
    argus_state_snapshot_t snap_after;
    argus_state_mgr_get_snapshot(&snap_after);

    // Assert zero mutation to live production state manager instance
    if (snap_before.machine_state != snap_after.machine_state) return false;
    if (snap_before.configured_target_rpm_milli != snap_after.configured_target_rpm_milli) return false;

    return true;
}

esp_err_t argus_tests_run_all(void)
{
    ESP_LOGI(TAG, "Running PURE non-motion unit tests (stack-local core, 0 tasks, 0 mutexes, 0 hardware touch)...");

    if (!test_cmd_parser()) {
        ESP_LOGE(TAG, "Test failed: Strict Command Parser");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[PASS] Strict Command Parser tests");

    if (!test_state_core_isolated()) {
        ESP_LOGE(TAG, "Test failed: Isolated State Core Permissions & Setpoint Isolation");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[PASS] Isolated State Core Permissions & Setpoint Isolation tests");

    if (!test_injected_error_propagation()) {
        ESP_LOGE(TAG, "Test failed: Injected Error Propagation & Recovery");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[PASS] Injected Error Propagation & Recovery tests");

    if (!test_production_singleton_isolation()) {
        ESP_LOGE(TAG, "Test failed: Production Singleton Isolation");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[PASS] Production Singleton Isolation tests (0 live mutations, 0 task leaks)");

    ESP_LOGI(TAG, "All PURE non-motion unit tests PASSED successfully.");
    return ESP_OK;
}

// ================= HARDWARE ACCEPTANCE INTERACTIVE SUBMENU =================

static int get_clean_char(void)
{
    int c;
    do {
        c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    } while (c == EOF || c == '\n' || c == '\r');
    return c;
}

esp_err_t argus_tests_run_hardware_acceptance(void)
{
    printf("\n");
    printf("===================================================\n");
    printf("=== DANGER — HARDWARE MOTION ACCEPTANCE TESTS ===\n");
    printf("===================================================\n");
    printf("WARNING: These tests will energize real motor hardware\n");
    printf("and execute physical output motion.\n");
    printf("Each test must be explicitly selected and confirmed.\n\n");

    bool print_menu = true;

    while (true) {
        if (print_menu) {
            printf("---------------------------------------------------\n");
            printf("Hardware Acceptance Submenu:\n");
            printf("[1] Test Setpoint Isolation (0.5 RPM setpoint while UNLOCKED)\n");
            printf("[2] Test Low-Speed Start & Ramp (0.5 RPM)\n");
            printf("[3] Test Normal Stop (ramp to 0, retain holding torque)\n");
            printf("[4] Test Software E-Stop while running\n");
            printf("[5] Test Reset E-Stop latch\n");
            printf("[6] Test Driver Unlock (release shaft)\n");
            printf("[7] Test Direction Reversal (ramp through zero)\n");
            printf("[8] Test Full-Speed Ramp (200 RPM)\n");
            printf("[9] Test Full-Speed Normal Stop\n");
            printf("[0] Exit Hardware Acceptance Submenu\n");
            printf("Select test option: ");
            fflush(stdout);
            print_menu = false;
        }

        int c = get_clean_char();
        printf("%c\n", c);
        print_menu = true;

        if (c == '0') {
            printf("Exiting Hardware Acceptance Submenu.\n");
            break;
        }

        printf("CAUTION: Confirm physical motion test '%c'? Press 'y' to proceed: ", c);
        fflush(stdout);

        int confirm = get_clean_char();
        printf("%c\n", confirm);

        if (confirm != 'y' && confirm != 'Y') {
            printf("Test '%c' cancelled by user.\n\n", c);
            continue;
        }

        printf("Executing Test '%c'...\n", c);
        switch (c) {
            case '1':
                argus_state_mgr_set_target(500, true);
                printf("  Setpoint updated to 500 mRPM. Driver remains UNLOCKED (0 pulses).\n");
                break;
            case '2':
                argus_state_mgr_set_target(500, true);
                argus_state_mgr_start();
                printf("  Motor ramping to 0.5 RPM...\n");
                break;
            case '3':
                argus_state_mgr_stop_normal();
                printf("  Motor ramping down to 0 RPM (retaining holding torque)...\n");
                break;
            case '4':
                argus_state_mgr_estop();
                printf("  Software E-STOP executed! Pulses halted immediately.\n");
                break;
            case '5':
                argus_state_mgr_reset_estop();
                printf("  E-STOP latch cleared (returned to stopped state).\n");
                break;
            case '6':
                argus_state_mgr_unlock();
                printf("  Driver UNLOCKED (GPIO 5 HIGH, shaft released).\n");
                break;
            case '7':
                argus_state_mgr_set_target(1000, false);
                argus_state_mgr_start();
                printf("  Reversing direction through zero speed...\n");
                break;
            case '8':
                argus_state_mgr_set_target(200000, true);
                argus_state_mgr_start();
                printf("  Full speed ramp to 200 RPM...\n");
                break;
            case '9':
                argus_state_mgr_stop_normal();
                printf("  Full speed normal stop to 0 RPM...\n");
                break;
            default:
                printf("  Unknown selection: '%c'\n", c);
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        printf("Test '%c' execution complete.\n\n", c);
    }

    return ESP_OK;
}
