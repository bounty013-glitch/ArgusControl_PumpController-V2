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

// Test 10: Authority Snapshot Coherence and Owner Permission Matrix
static esp_err_t test_authority_snapshot_coherence(void)
{
    TEST_ASSERT(argus_authority_mgr_init() == ESP_OK, "Auth init failed");
    TEST_ASSERT(argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SUPERVISORY, ARGUS_AUTH_OWNER_MQTT) == ESP_OK, "Auth set failed");

    argus_authority_snapshot_t snap;
    TEST_ASSERT(argus_authority_mgr_get_snapshot(&snap) == ESP_OK, "Auth snapshot failed");
    TEST_ASSERT(snap.mode == ARGUS_AUTHORITY_SUPERVISORY, "Auth mode mismatch");
    TEST_ASSERT(snap.owner == ARGUS_AUTH_OWNER_MQTT, "Auth owner mismatch");

    // MQTT supervisory allowed in SUPERVISORY mode
    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_MQTT_SUPERVISORY, ARGUS_CMD_TYPE_START) == true, "MQTT rejected in supervisory mode");

    // CLI motion rejected in SUPERVISORY mode
    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_CLI_DIAGNOSTIC, ARGUS_CMD_TYPE_START) == false, "CLI motion allowed in supervisory mode");

    // Internal safety E-stop ALWAYS allowed
    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_INTERNAL_SAFETY, ARGUS_CMD_TYPE_ESTOP) == true, "E-stop rejected");
    return ESP_OK;
}

// Test 11: Browser/CLI Owner Conflict Rejection
static esp_err_t test_browser_cli_owner_conflict_rejection(void)
{
    argus_authority_snapshot_t orig_snap;
    argus_authority_mgr_get_snapshot(&orig_snap);

    TEST_ASSERT(argus_authority_mgr_set_mode(ARGUS_AUTHORITY_LOCAL_SERVICE, ARGUS_AUTH_OWNER_BROWSER) == ESP_OK, "Set browser owner failed");

    argus_authority_snapshot_t snap;
    TEST_ASSERT(argus_authority_mgr_get_snapshot(&snap) == ESP_OK, "Get snapshot failed");

    // Browser allowed, CLI motion rejected
    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL, ARGUS_CMD_TYPE_SET_TARGET) == true, "Browser portal motion rejected");
    TEST_ASSERT(argus_authority_validate_permission(&snap, ARGUS_CMD_SRC_CLI_DIAGNOSTIC, ARGUS_CMD_TYPE_SET_TARGET) == false, "CLI motion allowed while browser owns authority");

    argus_authority_mgr_set_mode(orig_snap.mode, orig_snap.owner);
    return ESP_OK;
}

// Test 12: Command Router Dispatch Gate and Generation Invalidation
static esp_err_t test_command_router_dispatch_gate(void)
{
    argus_authority_snapshot_t orig_snap;
    argus_authority_mgr_get_snapshot(&orig_snap);

    TEST_ASSERT(argus_cmd_router_init() == ESP_OK, "Router init failed");
    TEST_ASSERT(argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SUPERVISORY, ARGUS_AUTH_OWNER_MQTT) == ESP_OK, "Set supervisory mode failed");

    argus_authority_snapshot_t snap;
    argus_authority_mgr_get_snapshot(&snap);

    argus_command_envelope_t valid_env = {
        .source = ARGUS_CMD_SRC_MQTT_SUPERVISORY,
        .command_type = ARGUS_CMD_TYPE_SET_TARGET,
        .authority_generation = snap.generation,
        .target_rpm_milli = 50000,
        .forward = true
    };
    TEST_ASSERT(argus_cmd_router_dispatch(&valid_env) == ESP_OK, "Valid router dispatch failed");

    // Stale generation counter rejected
    argus_command_envelope_t stale_env = valid_env;
    stale_env.authority_generation = snap.generation - 1;
    TEST_ASSERT(argus_cmd_router_dispatch(&stale_env) == ESP_ERR_INVALID_STATE, "Stale generation envelope accepted");

    argus_authority_mgr_set_mode(orig_snap.mode, orig_snap.owner);
    return ESP_OK;
}

esp_err_t argus_tests_4a_run_all(void)
{
    printf("\n--- Starting Phase 4A Pure Non-Motion Unit Tests ---\n");

    int passed = 0;
    int failed = 0;

#define RUN_TEST(fn) \
    do { \
        printf("Running %s... ", #fn); \
        if (fn() == ESP_OK) { \
            printf("[PASS]\n"); \
            passed++; \
        } else { \
            printf("[FAIL]\n"); \
            failed++; \
        } \
    } while (0)

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

    printf("--- Phase 4A Pure Unit Tests Summary: %d Passed, %d Failed ---\n\n", passed, failed);
    return (failed == 0) ? ESP_OK : ESP_FAIL;
}
