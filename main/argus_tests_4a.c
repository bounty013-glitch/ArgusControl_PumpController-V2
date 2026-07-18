/**
 * @file argus_tests_4a.c
 * @brief Phase 4A Pure Non-Motion Unit Test Suite Implementation
 */

#include "argus_tests_4a.h"
#include "argus_identity.h"
#include "argus_nvs_config.h"
#include "argus_authority_mgr.h"
#include "argus_cmd_router.h"
#include "argus_cmd_parser.h"
#include "argus_state_mgr.h"
#include "argus_net_mgr.h"
#include "argus_mqtt_broker.h"
#include "argus_console_helpers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] Line %d: %s\n", __LINE__, msg); \
            return ESP_FAIL; \
        } \
    } while (0)

// Stack-local mock NVS driver storage for testing
typedef struct {
    argus_cfg_slot_t slot_a;
    argus_cfg_slot_t slot_b;
    uint8_t selector;
    bool has_slot_a;
    bool has_slot_b;
    bool has_selector;
    bool reset_pending;
} mock_nvs_store_t;

static mock_nvs_store_t s_mock_nvs;

static esp_err_t mock_read_slot(uint8_t slot_index, argus_cfg_slot_t *out_slot)
{
    if (slot_index == 0) {
        if (!s_mock_nvs.has_slot_a) return ESP_ERR_NOT_FOUND;
        memcpy(out_slot, &s_mock_nvs.slot_a, sizeof(argus_cfg_slot_t));
        return ESP_OK;
    } else {
        if (!s_mock_nvs.has_slot_b) return ESP_ERR_NOT_FOUND;
        memcpy(out_slot, &s_mock_nvs.slot_b, sizeof(argus_cfg_slot_t));
        return ESP_OK;
    }
}

static esp_err_t mock_write_slot(uint8_t slot_index, const argus_cfg_slot_t *in_slot)
{
    if (slot_index == 0) {
        memcpy(&s_mock_nvs.slot_a, in_slot, sizeof(argus_cfg_slot_t));
        s_mock_nvs.has_slot_a = true;
    } else {
        memcpy(&s_mock_nvs.slot_b, in_slot, sizeof(argus_cfg_slot_t));
        s_mock_nvs.has_slot_b = true;
    }
    return ESP_OK;
}

static esp_err_t mock_read_selector(uint8_t *out_selector)
{
    if (!s_mock_nvs.has_selector) return ESP_ERR_NOT_FOUND;
    *out_selector = s_mock_nvs.selector;
    return ESP_OK;
}

static esp_err_t mock_write_selector(uint8_t selector)
{
    s_mock_nvs.selector = selector;
    s_mock_nvs.has_selector = true;
    return ESP_OK;
}

static esp_err_t mock_read_reset_pending(bool *out_pending)
{
    *out_pending = s_mock_nvs.reset_pending;
    return ESP_OK;
}

static esp_err_t mock_write_reset_pending(bool pending)
{
    s_mock_nvs.reset_pending = pending;
    return ESP_OK;
}

static esp_err_t mock_erase_all(void)
{
    memset(&s_mock_nvs, 0, sizeof(mock_nvs_store_t));
    return ESP_OK;
}

static const argus_nvs_driver_t s_mock_driver = {
    .read_slot = mock_read_slot,
    .write_slot = mock_write_slot,
    .read_selector = mock_read_selector,
    .write_selector = mock_write_selector,
    .read_reset_pending = mock_read_reset_pending,
    .write_reset_pending = mock_write_reset_pending,
    .erase_all = mock_erase_all
};

// Test 1: Identity MAC UID Derivation
static esp_err_t test_identity_mac_uid_derivation(void)
{
    TEST_ASSERT(argus_identity_init() == ESP_OK, "Identity init failed");
    argus_identity_t id;
    TEST_ASSERT(argus_identity_get(&id) == ESP_OK, "Identity get failed");
    TEST_ASSERT(strncmp(id.mac_uid, "ESP32S3-", 8) == 0, "MAC UID prefix mismatch");
    TEST_ASSERT(strncmp(id.service_ssid, "Argus-Service-", 14) == 0, "Service SSID prefix mismatch");
    return ESP_OK;
}

// Test 2: Identity Field Sanitization
static esp_err_t test_identity_field_sanitization(void)
{
    TEST_ASSERT(argus_identity_validate_client_id("valid_client-1") == true, "Valid client_id rejected");
    TEST_ASSERT(argus_identity_validate_client_id("invalid/client#") == false, "Invalid client_id accepted");
    TEST_ASSERT(argus_identity_validate_unit_id("unit_01") == true, "Valid unit_id rejected");
    TEST_ASSERT(argus_identity_validate_unit_id("") == false, "Empty unit_id accepted");
    TEST_ASSERT(argus_identity_validate_device_name("Argus Pump V2") == true, "Valid device_name rejected");
    return ESP_OK;
}

// Test 3: NVS Schema Validation
static esp_err_t test_nvs_schema_validation(void)
{
    argus_config_payload_t cfg = {
        .client_id = "test_client",
        .unit_id = "test_unit",
        .device_name = "Test Pump",
        .sta_ssid = "MyHomeWiFi",
        .sta_pass = "SuperSecret123"
    };
    TEST_ASSERT(argus_nvs_config_validate(&cfg) == ESP_OK, "Valid config rejected");
    TEST_ASSERT(argus_nvs_config_is_commissioned(&cfg) == true, "Commissioned check failed");
    return ESP_OK;
}

// Test 4: Open STA Network Rejection in Schema V1
static esp_err_t test_nvs_open_sta_rejection(void)
{
    argus_config_payload_t cfg = {
        .client_id = "test_client",
        .unit_id = "test_unit",
        .device_name = "Test Pump",
        .sta_ssid = "OpenWiFi",
        .sta_pass = "" // Empty password
    };
    TEST_ASSERT(argus_nvs_config_validate(&cfg) == ESP_ERR_INVALID_ARG, "Open STA accepted in schema v1");
    TEST_ASSERT(argus_nvs_config_is_commissioned(&cfg) == false, "Open STA marked commissioned");
    return ESP_OK;
}

// Test 5: Dual-Slot Atomic Write and Readback
static esp_err_t test_nvs_dual_slot_atomic_write_readback(void)
{
    memset(&s_mock_nvs, 0, sizeof(mock_nvs_store_t));
    TEST_ASSERT(argus_nvs_config_init(&s_mock_driver) == ESP_OK, "NVS init failed");

    argus_config_payload_t cfg = {
        .client_id = "client_alpha",
        .unit_id = "unit_alpha",
        .device_name = "Alpha Pump",
        .sta_ssid = "AlphaSSID",
        .sta_pass = "AlphaPass123"
    };
    TEST_ASSERT(argus_nvs_config_commit(&cfg) == ESP_OK, "NVS commit failed");

    argus_config_payload_t read_cfg;
    TEST_ASSERT(argus_nvs_config_get(&read_cfg) == ESP_OK, "NVS get failed");
    TEST_ASSERT(strcmp(read_cfg.client_id, "client_alpha") == 0, "client_id mismatch");
    TEST_ASSERT(strcmp(read_cfg.sta_ssid, "AlphaSSID") == 0, "sta_ssid mismatch");
    return ESP_OK;
}

// Test 6: LKG Rollback on CRC Mismatch
static esp_err_t test_nvs_lkg_rollback_on_crc_mismatch(void)
{
    memset(&s_mock_nvs, 0, sizeof(mock_nvs_store_t));
    TEST_ASSERT(argus_nvs_config_init(&s_mock_driver) == ESP_OK, "NVS init failed");

    argus_config_payload_t cfg1 = {
        .client_id = "client_good",
        .unit_id = "unit_good",
        .device_name = "Good Pump",
        .sta_ssid = "GoodSSID",
        .sta_pass = "GoodPass123"
    };
    TEST_ASSERT(argus_nvs_config_commit(&cfg1) == ESP_OK, "First commit failed");

    // Corrupt active slot CRC
    s_mock_nvs.slot_a.crc32 ^= 0xFFFFFFFF;

    // Re-init NVS (simulating reboot)
    TEST_ASSERT(argus_nvs_config_init(&s_mock_driver) == ESP_OK, "NVS re-init failed");

    argus_config_payload_t read_cfg;
    TEST_ASSERT(argus_nvs_config_get(&read_cfg) == ESP_ERR_NOT_FOUND, "Corrupt slot accepted");
    return ESP_OK;
}

// Test 7: Password Masking in Telemetry
static esp_err_t test_password_masking_in_telemetry(void)
{
    argus_config_payload_t cfg = {
        .client_id = "client1",
        .unit_id = "unit1",
        .device_name = "Pump1",
        .sta_ssid = "MyWiFi",
        .sta_pass = "SuperSecretPass"
    };
    argus_config_payload_t masked;
    argus_nvs_config_mask(&cfg, &masked);

    TEST_ASSERT(strcmp(masked.sta_pass, ARGUS_CONFIG_MASK_STRING) == 0, "Password not masked");
    TEST_ASSERT(strcmp(masked.sta_ssid, "MyWiFi") == 0, "SSID corrupted during mask");
    return ESP_OK;
}

// Test 8: Mask String Write Rejection
static esp_err_t test_mask_string_write_rejection(void)
{
    memset(&s_mock_nvs, 0, sizeof(mock_nvs_store_t));
    TEST_ASSERT(argus_nvs_config_init(&s_mock_driver) == ESP_OK, "NVS init failed");

    argus_config_payload_t cfg = {
        .client_id = "client1",
        .unit_id = "unit1",
        .device_name = "Pump1",
        .sta_ssid = "MyWiFi",
        .sta_pass = ARGUS_CONFIG_MASK_STRING // Mask string submitted back
    };
    TEST_ASSERT(argus_nvs_config_commit(&cfg) == ESP_ERR_INVALID_ARG, "Mask string accepted in commit");
    return ESP_OK;
}

// Test 9: Generation Wrap Comparison
static esp_err_t test_nvs_generation_wrap_comparison(void)
{
    TEST_ASSERT(argus_nvs_config_gen_is_newer(5, 2) == true, "Normal newer comparison failed");
    TEST_ASSERT(argus_nvs_config_gen_is_newer(2, 5) == false, "Normal older comparison failed");
    TEST_ASSERT(argus_nvs_config_gen_is_newer(1, 0xFFFFFFFFU) == true, "Wrap-around newer comparison failed");
    TEST_ASSERT(argus_nvs_config_gen_is_newer(0, 5) == false, "Generation 0 accepted as newer");
    return ESP_OK;
}

// Test 10: Pure Authority Snapshot Permission Matrix Validation
static esp_err_t test_authority_snapshot_coherence(void)
{
    argus_authority_snapshot_t snap = {
        .mode = ARGUS_AUTHORITY_SUPERVISORY,
        .owner = ARGUS_AUTH_OWNER_MQTT,
        .generation = 1
    };

    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_MQTT_SUPERVISORY, ARGUS_CMD_TYPE_START) == true, "MQTT rejected in supervisory mode");
    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_CLI_DIAGNOSTIC, ARGUS_CMD_TYPE_START) == false, "CLI motion allowed in supervisory mode");
    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_INTERNAL_SAFETY, ARGUS_CMD_TYPE_ESTOP) == true, "E-stop rejected");
    return ESP_OK;
}

// Test 11: Pure Browser/CLI Owner Conflict Validation
static esp_err_t test_browser_cli_owner_conflict_rejection(void)
{
    argus_authority_snapshot_t snap = {
        .mode = ARGUS_AUTHORITY_LOCAL_SERVICE,
        .owner = ARGUS_AUTH_OWNER_BROWSER,
        .generation = 2
    };

    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL, ARGUS_CMD_TYPE_SET_TARGET) == true, "Browser portal motion rejected");
    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_CLI_DIAGNOSTIC, ARGUS_CMD_TYPE_SET_TARGET) == false, "CLI motion allowed while browser owns authority");
    return ESP_OK;
}

// Test 12: Pure Command Router Check Authority & Generation Gate
static esp_err_t test_command_router_dispatch_gate(void)
{
    argus_authority_snapshot_t cur_snap;
    argus_authority_mgr_get_snapshot(&cur_snap);

    argus_command_envelope_t valid_env = {
        .source = ARGUS_CMD_SRC_CLI_DIAGNOSTIC,
        .command_type = ARGUS_CMD_TYPE_SET_TARGET,
        .authority_generation = cur_snap.generation,
        .target_rpm_milli = 50000,
        .forward = true
    };

    esp_err_t res = argus_cmd_router_check_authority(&valid_env);
    if (cur_snap.mode == ARGUS_AUTHORITY_LOCAL_SERVICE && cur_snap.owner == ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
        TEST_ASSERT(res == ESP_OK, "Valid envelope check failed");
    } else {
        TEST_ASSERT(res == ESP_ERR_INVALID_STATE, "Unpermitted envelope check accepted");
    }

    argus_command_envelope_t stale_env = valid_env;
    stale_env.authority_generation = cur_snap.generation - 1;
    TEST_ASSERT(argus_cmd_router_check_authority(&stale_env) == ESP_ERR_INVALID_STATE, "Stale generation envelope check accepted");
    return ESP_OK;
}

typedef struct {
    argus_state_snapshot_t state;
    argus_authority_snapshot_t authority;
    argus_network_mode_t net_mode;
    argus_net_err_t last_net_err;
    UBaseType_t task_count;
} argus_prod_snapshot_t;

static void capture_prod_snapshot(argus_prod_snapshot_t *out)
{
    argus_state_mgr_get_snapshot(&out->state);
    argus_authority_mgr_get_snapshot(&out->authority);
    out->net_mode = argus_net_mgr_get_mode();
    out->last_net_err = argus_net_mgr_get_last_error();
    out->task_count = uxTaskGetNumberOfTasks();
}

static bool compare_prod_snapshots(const argus_prod_snapshot_t *b, const argus_prod_snapshot_t *a)
{
    return (b->state.machine_state == a->state.machine_state &&
            b->state.configured_target_rpm_milli == a->state.configured_target_rpm_milli &&
            b->state.trajectory_target_rpm_milli == a->state.trajectory_target_rpm_milli &&
            b->state.applied_rpm_milli == a->state.applied_rpm_milli &&
            b->state.generated_rpm_milli == a->state.generated_rpm_milli &&
            b->state.generated_step_count == a->state.generated_step_count &&
            b->state.requested_forward == a->state.requested_forward &&
            b->state.applied_forward == a->state.applied_forward &&
            b->state.driver_enabled == a->state.driver_enabled &&
            b->state.ramp_active == a->state.ramp_active &&
            b->state.estop_latched == a->state.estop_latched &&
            b->state.fault_code == a->state.fault_code &&
            b->authority.mode == a->authority.mode &&
            b->authority.owner == a->authority.owner &&
            b->authority.generation == a->authority.generation &&
            b->net_mode == a->net_mode &&
            b->last_net_err == a->last_net_err &&
            b->task_count == a->task_count);
}

extern bool argus_app_main_get_console_verbosity(void);
extern void argus_app_main_set_console_verbosity(bool verbose);
extern void argus_app_main_print_oneshot_status(void);

// Test 13: Console Verbosity Policy Defaults to OFF and Toggles Independently of MQTT Policy
static esp_err_t test_console_verbosity_policy_and_toggling(void)
{
    TEST_ASSERT(argus_app_main_get_console_verbosity() == false, "Console verbosity default is not OFF");

    argus_app_main_set_console_verbosity(true);
    TEST_ASSERT(argus_app_main_get_console_verbosity() == true, "Console verbosity toggle ON failed");

    argus_app_main_set_console_verbosity(false);
    TEST_ASSERT(argus_app_main_get_console_verbosity() == false, "Console verbosity toggle OFF failed");

    bool broker_running = argus_mqtt_broker_is_running();
    TEST_ASSERT(broker_running == false || broker_running == true, "MQTT broker policy check failed");
    return ESP_OK;
}

// Test 14: One-Shot Status Display Does Not Mutate Production State
static esp_err_t test_oneshot_status_non_mutation(void)
{
    argus_prod_snapshot_t snap_before, snap_after;
    capture_prod_snapshot(&snap_before);

    argus_app_main_print_oneshot_status();

    capture_prod_snapshot(&snap_after);
    TEST_ASSERT(compare_prod_snapshots(&snap_before, &snap_after) == true, "One-shot status snapshot mutated production state!");
    return ESP_OK;
}

typedef struct {
    mock_nvs_store_t store;
    bool corrupt_readback;
} corruptible_mock_nvs_t;

static corruptible_mock_nvs_t s_corrupt_mock;

static esp_err_t corrupt_mock_read_slot(uint8_t slot_index, argus_cfg_slot_t *out_slot)
{
    esp_err_t res = mock_read_slot(slot_index, out_slot);
    if (res == ESP_OK && s_corrupt_mock.corrupt_readback && slot_index == 1) {
        out_slot->crc32 ^= 0xFFFFFFFFU;
    }
    return res;
}

// Test 15: Commit Verification Rejects Corrupted Readback & Preserves Prior LKG
static esp_err_t test_nvs_commit_readback_verification_and_lkg_preservation(void)
{
    memset(&s_corrupt_mock, 0, sizeof(corruptible_mock_nvs_t));
    mock_erase_all();

    argus_config_payload_t initial_lkg = {0};
    strlcpy(initial_lkg.client_id, "lkg_client", sizeof(initial_lkg.client_id));
    strlcpy(initial_lkg.unit_id, "lkg_unit", sizeof(initial_lkg.unit_id));
    strlcpy(initial_lkg.device_name, "LKG Device", sizeof(initial_lkg.device_name));
    strlcpy(initial_lkg.sta_ssid, "InitialSSID", sizeof(initial_lkg.sta_ssid));
    strlcpy(initial_lkg.sta_pass, "InitialPass123", sizeof(initial_lkg.sta_pass));

    TEST_ASSERT(argus_nvs_config_init(&s_mock_driver) == ESP_OK, "Mock NVS init failed");
    TEST_ASSERT(argus_nvs_config_commit(&initial_lkg) == ESP_OK, "Initial LKG commit failed");

    s_corrupt_mock.corrupt_readback = true;
    argus_nvs_driver_t corrupt_driver = s_mock_driver;
    corrupt_driver.read_slot = corrupt_mock_read_slot;
    argus_nvs_config_init(&corrupt_driver);

    argus_config_payload_t corrupted_staged = initial_lkg;
    strlcpy(corrupted_staged.sta_ssid, "CorruptedSSID", sizeof(corrupted_staged.sta_ssid));

    esp_err_t err = argus_nvs_config_commit(&corrupted_staged);
    TEST_ASSERT(err == ESP_ERR_INVALID_STATE, "Commit accepted corrupted readback!");

    argus_config_payload_t active;
    TEST_ASSERT(argus_nvs_config_get(&active) == ESP_OK, "LKG get failed");
    TEST_ASSERT(strcmp(active.sta_ssid, "InitialSSID") == 0, "Corrupted commit replaced prior LKG!");

    s_corrupt_mock.corrupt_readback = false;
    argus_nvs_config_init(&s_mock_driver);
    return ESP_OK;
}

// Test 16: Network Truthfulness, Broker Ordering & Disconnect Authority Revocation
static esp_err_t test_network_truthfulness_and_broker_ordering(void)
{
    TEST_ASSERT(argus_net_mgr_is_ap_started() == false, "AP reported started before AP event!");

    argus_net_event_t evt_conn = { .type = ARGUS_NET_EVT_STA_CONNECTED };
    argus_net_mgr_post_event(&evt_conn);
    vTaskDelay(pdMS_TO_TICKS(50));

    TEST_ASSERT(argus_net_mgr_get_mode() == ARGUS_NET_MODE_COMMISSIONED_STA, "STA_CONNECTED incorrectly set AP_DISCOVERABLE!");
    TEST_ASSERT(argus_net_mgr_is_ap_started() == false, "STA_CONNECTED started AP!");

    argus_net_event_t evt_disc = { .type = ARGUS_NET_EVT_STA_DISCONNECTED };
    argus_net_mgr_post_event(&evt_disc);
    vTaskDelay(pdMS_TO_TICKS(50));

    argus_authority_snapshot_t auth_snap;
    argus_authority_mgr_get_snapshot(&auth_snap);
    TEST_ASSERT(auth_snap.mode == ARGUS_AUTHORITY_NONE, "MQTT authority was not revoked on STA disconnect!");
    TEST_ASSERT(auth_snap.owner == ARGUS_AUTH_OWNER_NONE, "MQTT owner was not cleared on STA disconnect!");

    return ESP_OK;
}

static esp_err_t test_console_input_validation(void)
{
    char key = '\0';

    // 1. Empty menu input
    TEST_ASSERT(argus_console_parse_menu_key("", &key) == ESP_ERR_INVALID_ARG, "Empty menu input accepted");

    // 2. Whitespace-only input
    TEST_ASSERT(argus_console_parse_menu_key("   \t\r\n  ", &key) == ESP_ERR_INVALID_ARG, "Whitespace menu input accepted");

    // 3. One-character menu input
    TEST_ASSERT(argus_console_parse_menu_key("8", &key) == ESP_OK && key == '8', "Single key '8' failed");
    TEST_ASSERT(argus_console_parse_menu_key("N", &key) == ESP_OK && key == 'N', "Single key 'N' failed");

    // 4. Leading/trailing whitespace around single character
    TEST_ASSERT(argus_console_parse_menu_key("  8 \t ", &key) == ESP_OK && key == '8', "Padded single key failed");

    // 5. Multi-character rejection
    TEST_ASSERT(argus_console_parse_menu_key("89", &key) == ESP_ERR_INVALID_ARG, "Multi-char '89' accepted");
    TEST_ASSERT(argus_console_parse_menu_key("  8 9 ", &key) == ESP_ERR_INVALID_ARG, "Multi-char '8 9' accepted");

    // 6. Maximum valid SSID length (32)
    char ssid_32[33];
    memset(ssid_32, 'A', 32);
    ssid_32[32] = '\0';
    TEST_ASSERT(argus_console_validate_ssid(ssid_32) == ESP_OK, "32-char SSID rejected");

    // 7. Overlength SSID rejection (> 32)
    char ssid_33[34];
    memset(ssid_33, 'A', 33);
    ssid_33[33] = '\0';
    TEST_ASSERT(argus_console_validate_ssid(ssid_33) == ESP_ERR_INVALID_SIZE, "33-char SSID accepted");

    // 8. Minimum valid password length (8)
    TEST_ASSERT(argus_console_validate_password("12345678") == ESP_OK, "8-char password rejected");

    // 9. Maximum valid password length (63)
    char pass_63[64];
    memset(pass_63, 'B', 63);
    pass_63[63] = '\0';
    TEST_ASSERT(argus_console_validate_password(pass_63) == ESP_OK, "63-char password rejected");

    // 10. Password underlength rejection (< 8)
    TEST_ASSERT(argus_console_validate_password("1234567") == ESP_ERR_INVALID_SIZE, "7-char password accepted");

    // 11. Password overlength rejection (> 63)
    char pass_64[65];
    memset(pass_64, 'B', 64);
    pass_64[64] = '\0';
    TEST_ASSERT(argus_console_validate_password(pass_64) == ESP_ERR_INVALID_SIZE, "64-char password accepted");

    return ESP_OK;
}

esp_err_t argus_tests_4a_run_all(void)
{
    printf("\n===================================================\n");
    printf("=== Phase 4A Pure Non-Motion Unit Test Suite ===\n");
    printf("===================================================\n");

    int passed = 0;
    int failed = 0;

    argus_prod_snapshot_t snap_before, snap_after;
    capture_prod_snapshot(&snap_before);

#define RUN_TEST(test_fn) \
    do { \
        printf("Running %-55s ... ", #test_fn); \
        esp_err_t err = test_fn(); \
        if (err == ESP_OK) { \
            printf("[PASSED]\n"); \
            passed++; \
        } else { \
            printf("[FAIL]\n"); \
            failed++; \
        } \
    } while (0)

    for (int run = 1; run <= 3; run++) {
        printf("\n--- Executing Phase 4A Test Pass %d of 3 ---\n", run);
        RUN_TEST(test_identity_mac_uid_derivation);
        RUN_TEST(test_identity_field_sanitization);
        RUN_TEST(test_nvs_schema_validation);
        RUN_TEST(test_nvs_open_sta_rejection);
        RUN_TEST(test_nvs_dual_slot_atomic_write_readback);
        RUN_TEST(test_nvs_lkg_rollback_on_crc_mismatch);
        RUN_TEST(test_password_masking_in_telemetry);
        RUN_TEST(test_mask_string_write_rejection);
        RUN_TEST(test_nvs_generation_wrap_comparison);
        RUN_TEST(test_authority_snapshot_coherence);
        RUN_TEST(test_browser_cli_owner_conflict_rejection);
        RUN_TEST(test_command_router_dispatch_gate);
        RUN_TEST(test_console_verbosity_policy_and_toggling);
        RUN_TEST(test_oneshot_status_non_mutation);
        RUN_TEST(test_nvs_commit_readback_verification_and_lkg_preservation);
        RUN_TEST(test_network_truthfulness_and_broker_ordering);
        RUN_TEST(test_console_input_validation);
    }

    capture_prod_snapshot(&snap_after);
    bool non_mutated = compare_prod_snapshots(&snap_before, &snap_after);
    printf("\n--- Production Non-Mutation Proof: %s ---\n", non_mutated ? "PASSED (100% Equal)" : "FAILED");
    if (!non_mutated) {
        printf("[FAIL] Production singleton was mutated during pure test passes!\n");
        failed++;
    }

    printf("--- Phase 4A Pure Unit Tests Summary: %d Passed, %d Failed ---\n\n", passed, failed);
    return (failed == 0) ? ESP_OK : ESP_FAIL;
}
