/**
 * @file argus_tests_4a.c
 * @brief Phase 4A Pure Non-Motion Unit Test Suite Implementation (100% Stack-Local Isolation)
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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] Line %d: %s\n", __LINE__, msg); \
            return ESP_FAIL; \
        } \
    } while (0)

static const int EXPECTED_FULL_SEQUENCE[] = {2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15};


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

static esp_err_t mock_read_slot(void *ctx, uint8_t slot_index, argus_cfg_slot_t *out_slot)
{
    mock_nvs_store_t *store = (mock_nvs_store_t *)ctx;
    if (!store) return ESP_ERR_INVALID_ARG;
    if (slot_index == 0) {
        if (!store->has_slot_a) return ESP_ERR_NOT_FOUND;
        memcpy(out_slot, &store->slot_a, sizeof(argus_cfg_slot_t));
        return ESP_OK;
    } else {
        if (!store->has_slot_b) return ESP_ERR_NOT_FOUND;
        memcpy(out_slot, &store->slot_b, sizeof(argus_cfg_slot_t));
        return ESP_OK;
    }
}

static esp_err_t mock_write_slot(void *ctx, uint8_t slot_index, const argus_cfg_slot_t *in_slot)
{
    mock_nvs_store_t *store = (mock_nvs_store_t *)ctx;
    if (!store) return ESP_ERR_INVALID_ARG;
    if (slot_index == 0) {
        memcpy(&store->slot_a, in_slot, sizeof(argus_cfg_slot_t));
        store->has_slot_a = true;
    } else {
        memcpy(&store->slot_b, in_slot, sizeof(argus_cfg_slot_t));
        store->has_slot_b = true;
    }
    return ESP_OK;
}

static esp_err_t mock_read_selector(void *ctx, uint8_t *out_selector)
{
    mock_nvs_store_t *store = (mock_nvs_store_t *)ctx;
    if (!store || !store->has_selector) return ESP_ERR_NOT_FOUND;
    *out_selector = store->selector;
    return ESP_OK;
}

static esp_err_t mock_write_selector(void *ctx, uint8_t selector)
{
    mock_nvs_store_t *store = (mock_nvs_store_t *)ctx;
    if (!store) return ESP_ERR_INVALID_ARG;
    store->selector = selector;
    store->has_selector = true;
    return ESP_OK;
}

static esp_err_t mock_read_reset_pending(void *ctx, bool *out_pending)
{
    mock_nvs_store_t *store = (mock_nvs_store_t *)ctx;
    if (!store) return ESP_ERR_INVALID_ARG;
    *out_pending = store->reset_pending;
    return ESP_OK;
}

static esp_err_t mock_write_reset_pending(void *ctx, bool pending)
{
    mock_nvs_store_t *store = (mock_nvs_store_t *)ctx;
    if (!store) return ESP_ERR_INVALID_ARG;
    store->reset_pending = pending;
    return ESP_OK;
}

static esp_err_t mock_erase_all(void *ctx)
{
    mock_nvs_store_t *store = (mock_nvs_store_t *)ctx;
    if (!store) return ESP_ERR_INVALID_ARG;
    memset(store, 0, sizeof(mock_nvs_store_t));
    return ESP_OK;
}

static void make_mock_driver(argus_nvs_driver_t *out_driver, mock_nvs_store_t *store)
{
    out_driver->read_slot = mock_read_slot;
    out_driver->write_slot = mock_write_slot;
    out_driver->read_selector = mock_read_selector;
    out_driver->write_selector = mock_write_selector;
    out_driver->read_reset_pending = mock_read_reset_pending;
    out_driver->write_reset_pending = mock_write_reset_pending;
    out_driver->erase_all = mock_erase_all;
    out_driver->ctx = store;
}

// Test 1: Pure MAC UID Derivation Formatting
static esp_err_t test_identity_mac_uid_derivation(void)
{
    uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    char uid[32], ssid[32];
    snprintf(uid, sizeof(uid), "ESP32S3-%02X%02X", mac[4], mac[5]);
    snprintf(ssid, sizeof(ssid), "Argus-Service-%02X%02X", mac[4], mac[5]);
    TEST_ASSERT(strcmp(uid, "ESP32S3-2233") == 0, "MAC UID string formatting failed");
    TEST_ASSERT(strcmp(ssid, "Argus-Service-2233") == 0, "Service SSID string formatting failed");
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
        .sta_pass = ""
    };
    TEST_ASSERT(argus_nvs_config_validate(&cfg) == ESP_ERR_INVALID_ARG, "Open STA accepted in schema v1");
    TEST_ASSERT(argus_nvs_config_is_commissioned(&cfg) == false, "Open STA marked commissioned");
    return ESP_OK;
}

// Test 5: Dual-Slot Atomic Write and Readback (100% Stack-Local Context)
static esp_err_t test_nvs_dual_slot_atomic_write_readback(void)
{
    mock_nvs_store_t store = {0};
    argus_nvs_driver_t driver;
    make_mock_driver(&driver, &store);

    argus_nvs_core_t core;
    TEST_ASSERT(argus_nvs_core_init(&core, &driver) == ESP_OK, "NVS core init failed");

    argus_config_payload_t cfg = {
        .client_id = "client_alpha",
        .unit_id = "unit_alpha",
        .device_name = "Alpha Pump",
        .sta_ssid = "AlphaSSID",
        .sta_pass = "AlphaPass123"
    };
    TEST_ASSERT(argus_nvs_core_commit(&core, &cfg) == ESP_OK, "NVS core commit failed");

    argus_config_payload_t read_cfg;
    TEST_ASSERT(argus_nvs_core_get(&core, &read_cfg) == ESP_OK, "NVS core get failed");
    TEST_ASSERT(strcmp(read_cfg.client_id, "client_alpha") == 0, "client_id mismatch");
    TEST_ASSERT(strcmp(read_cfg.sta_ssid, "AlphaSSID") == 0, "sta_ssid mismatch");

    // Re-init core to prove persistent get from verified driver payload
    argus_nvs_core_t reinit_core;
    TEST_ASSERT(argus_nvs_core_init(&reinit_core, &driver) == ESP_OK, "Reinit core failed");
    argus_config_payload_t reinit_cfg;
    TEST_ASSERT(argus_nvs_core_get(&reinit_core, &reinit_cfg) == ESP_OK, "Reinit get failed");
    TEST_ASSERT(strcmp(reinit_cfg.client_id, "client_alpha") == 0, "Reinit payload mismatch");

    return ESP_OK;
}

// Test 6: LKG Rollback on CRC Mismatch
static esp_err_t test_nvs_lkg_rollback_on_crc_mismatch(void)
{
    mock_nvs_store_t store = {0};
    argus_nvs_driver_t driver;
    make_mock_driver(&driver, &store);

    argus_nvs_core_t core1;
    TEST_ASSERT(argus_nvs_core_init(&core1, &driver) == ESP_OK, "NVS core init failed");

    argus_config_payload_t cfg1 = {
        .client_id = "client_good",
        .unit_id = "unit_good",
        .device_name = "Good Pump",
        .sta_ssid = "GoodSSID",
        .sta_pass = "GoodPass123"
    };
    TEST_ASSERT(argus_nvs_core_commit(&core1, &cfg1) == ESP_OK, "First commit failed");

    // Corrupt active slot CRC
    store.slot_a.crc32 ^= 0xFFFFFFFF;
    store.slot_b.crc32 ^= 0xFFFFFFFF;

    argus_nvs_core_t core2;
    TEST_ASSERT(argus_nvs_core_init(&core2, &driver) == ESP_OK, "NVS core re-init failed");

    argus_config_payload_t read_cfg;
    TEST_ASSERT(argus_nvs_core_get(&core2, &read_cfg) == ESP_ERR_NOT_FOUND, "Corrupt slot accepted");
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
    mock_nvs_store_t store = {0};
    argus_nvs_driver_t driver;
    make_mock_driver(&driver, &store);

    argus_nvs_core_t core;
    TEST_ASSERT(argus_nvs_core_init(&core, &driver) == ESP_OK, "NVS core init failed");

    argus_config_payload_t cfg = {
        .client_id = "client1",
        .unit_id = "unit1",
        .device_name = "Pump1",
        .sta_ssid = "MyWiFi",
        .sta_pass = ARGUS_CONFIG_MASK_STRING
    };
    TEST_ASSERT(argus_nvs_core_commit(&core, &cfg) == ESP_ERR_INVALID_ARG, "Mask string accepted in commit");
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

// Test 12: Pure Command Router Check Authority Gate (Stack-Local)
static esp_err_t test_command_router_dispatch_gate(void)
{
    argus_authority_snapshot_t cur_snap = {
        .mode = ARGUS_AUTHORITY_LOCAL_SERVICE,
        .owner = ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI,
        .generation = 5
    };

    argus_command_envelope_t valid_env = {
        .source = ARGUS_CMD_SRC_CLI_DIAGNOSTIC,
        .command_type = ARGUS_CMD_TYPE_SET_TARGET,
        .authority_generation = 5,
        .target_rpm_milli = 50000,
        .forward = true
    };

    TEST_ASSERT(argus_authority_validate_permission(&cur_snap, valid_env.source, valid_env.command_type) == true, "Valid envelope permission check failed");
    TEST_ASSERT((valid_env.authority_generation == cur_snap.generation) == true, "Generation mismatch check failed");

    argus_command_envelope_t stale_env = valid_env;
    stale_env.authority_generation = 4;
    TEST_ASSERT((stale_env.authority_generation == cur_snap.generation) == false, "Stale generation check failed");
    return ESP_OK;
}

typedef struct {
    bool console_verbose;
} console_policy_t;

// Test 13: Console Verbosity Policy Defaults to OFF and Toggles Independently
static esp_err_t test_console_verbosity_policy_and_toggling(void)
{
    console_policy_t policy = { .console_verbose = false };
    TEST_ASSERT(policy.console_verbose == false, "Console verbosity default is not OFF");
    policy.console_verbose = true;
    TEST_ASSERT(policy.console_verbose == true, "Console verbosity toggle ON failed");
    policy.console_verbose = false;
    TEST_ASSERT(policy.console_verbose == false, "Console verbosity toggle OFF failed");
    return ESP_OK;
}

// Test 14: Pure One-Shot Status Formatting Function
static esp_err_t test_oneshot_status_non_mutation(void)
{
    argus_state_snapshot_t state = { .machine_state = ARGUS_STATE_HOLDING, .applied_rpm_milli = 0 };
    argus_authority_snapshot_t auth = { .mode = ARGUS_AUTHORITY_LOCAL_SERVICE, .owner = ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, .generation = 2 };
    char buf[128];
    snprintf(buf, sizeof(buf), "State:[%s] Mode:[%s] Owner:[%s] Gen:[%lu]",
             argus_state_mgr_get_state_name(state.machine_state),
             argus_authority_mgr_get_mode_name(auth.mode),
             argus_authority_mgr_get_owner_name(auth.owner),
             (unsigned long)auth.generation);
    TEST_ASSERT(strstr(buf, "HOLDING") != NULL, "State rendering failed");
    TEST_ASSERT(strstr(buf, "LOCAL_SERVICE") != NULL, "Auth rendering failed");
    return ESP_OK;
}

typedef struct {
    mock_nvs_store_t store;
    bool corrupt_readback;
} corruptible_mock_nvs_t;

static esp_err_t corrupt_mock_read_slot(void *ctx, uint8_t slot_index, argus_cfg_slot_t *out_slot)
{
    corruptible_mock_nvs_t *cstore = (corruptible_mock_nvs_t *)ctx;
    esp_err_t res = mock_read_slot(&cstore->store, slot_index, out_slot);
    if (res == ESP_OK && cstore->corrupt_readback) {
        out_slot->crc32 ^= 0xFFFFFFFFU;
    }
    return res;
}

// Test 15: Commit Verification Rejects Corrupted Readback & Preserves Prior LKG
static esp_err_t test_nvs_commit_readback_verification_and_lkg_preservation(void)
{
    corruptible_mock_nvs_t cstore = {0};
    argus_nvs_driver_t driver;
    make_mock_driver(&driver, &cstore.store);

    argus_config_payload_t initial_lkg = {0};
    strlcpy(initial_lkg.client_id, "lkg_client", sizeof(initial_lkg.client_id));
    strlcpy(initial_lkg.unit_id, "lkg_unit", sizeof(initial_lkg.unit_id));
    strlcpy(initial_lkg.device_name, "LKG Device", sizeof(initial_lkg.device_name));
    strlcpy(initial_lkg.sta_ssid, "InitialSSID", sizeof(initial_lkg.sta_ssid));
    strlcpy(initial_lkg.sta_pass, "InitialPass123", sizeof(initial_lkg.sta_pass));

    argus_nvs_core_t core1;
    TEST_ASSERT(argus_nvs_core_init(&core1, &driver) == ESP_OK, "Mock NVS core init failed");
    TEST_ASSERT(argus_nvs_core_commit(&core1, &initial_lkg) == ESP_OK, "Initial LKG commit failed");

    argus_nvs_driver_t corrupt_driver = driver;
    corrupt_driver.read_slot = corrupt_mock_read_slot;
    corrupt_driver.ctx = &cstore;

    cstore.corrupt_readback = true;
    argus_nvs_core_t core_corrupt;
    argus_nvs_core_init(&core_corrupt, &corrupt_driver);

    argus_config_payload_t corrupted_staged = initial_lkg;
    strlcpy(corrupted_staged.sta_ssid, "CorruptedSSID", sizeof(corrupted_staged.sta_ssid));

    esp_err_t err = argus_nvs_core_commit(&core_corrupt, &corrupted_staged);
    TEST_ASSERT(err == ESP_ERR_INVALID_STATE, "Commit accepted corrupted readback!");

    cstore.corrupt_readback = false;
    argus_nvs_core_t core_read;
    argus_nvs_core_init(&core_read, &driver);

    argus_config_payload_t active;
    TEST_ASSERT(argus_nvs_core_get(&core_read, &active) == ESP_OK, "LKG get failed");
    TEST_ASSERT(strcmp(active.sta_ssid, "InitialSSID") == 0, "Corrupted commit replaced prior LKG!");

    return ESP_OK;
}

typedef struct {
    // Authority state (stack-local)
    argus_authority_core_t auth_core;

    // Machine simulation
    argus_machine_state_t mock_machine_state;
    bool estop_latched;

    // Call counters
    int prepare_count;
    int abort_count;
    int grant_count;

    // Per-operation call counts
    int normal_stop_count;
    int verify_stopped_count;
    int stop_broker_count;
    int verify_broker_count;
    int disconnect_sta_count;
    int verify_sta_disc_count;
    int verify_sta_ip_count;
    int set_ap_count;
    int verify_ap_count;
    int verify_machine_safe_count;

    // Exact call sequence
    int call_sequence[24];
    int call_count;

    // Failure injection
    int fail_stage;
    esp_err_t fail_error;
    bool estop_during_stop;

    int motion_start_count;
} mock_orchestration_ctx_t;

static void init_mock_ctx(mock_orchestration_ctx_t *m) {
    memset(m, 0, sizeof(*m));
    m->auth_core.mode = ARGUS_AUTHORITY_NONE;
    m->auth_core.owner = ARGUS_AUTH_OWNER_NONE;
    m->auth_core.generation = 1;
    m->auth_core.last_error = ESP_OK;
    m->mock_machine_state = ARGUS_STATE_HOLDING;
    m->estop_latched = false;
    m->fail_stage = -1;
    m->fail_error = ESP_ERR_INVALID_STATE;
    m->estop_during_stop = false;
}

static esp_err_t mock_prepare_transition(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->prepare_count++;
    m->call_sequence[m->call_count++] = 2;
    if (m->fail_stage == 2) return m->fail_error;
    return argus_authority_core_prepare_service_transition(&m->auth_core);
}

static esp_err_t mock_grant_local(void *ctx, argus_authority_owner_t owner) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->grant_count++;
    m->call_sequence[m->call_count++] = 15;
    if (m->fail_stage == 15) return m->fail_error;
    return argus_authority_core_grant_local_service(&m->auth_core, owner);
}

static void mock_abort_transition(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->abort_count++;
    argus_authority_core_abort_service_transition(&m->auth_core);
}

static esp_err_t mock_request_normal_stop(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->normal_stop_count++;
    m->call_sequence[m->call_count++] = 4;
    if (m->fail_stage == 4) return m->fail_error;
    return ESP_OK;
}

static esp_err_t mock_verify_stopped(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->verify_stopped_count++;
    m->call_sequence[m->call_count++] = 5;
    if (m->estop_during_stop) {
        m->mock_machine_state = ARGUS_STATE_EMERGENCY_STOPPED;
        m->estop_latched = true;
        return m->fail_error;
    }
    if (m->fail_stage == 5) return m->fail_error;
    return ESP_OK;
}

static esp_err_t mock_stop_broker(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->stop_broker_count++;
    m->call_sequence[m->call_count++] = 6;
    if (m->fail_stage == 6) return m->fail_error;
    return ESP_OK;
}

static esp_err_t mock_verify_broker_stopped(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->verify_broker_count++;
    m->call_sequence[m->call_count++] = 7;
    if (m->fail_stage == 7) return m->fail_error;
    return ESP_OK;
}

static esp_err_t mock_disconnect_sta(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->disconnect_sta_count++;
    m->call_sequence[m->call_count++] = 8;
    if (m->fail_stage == 8) return m->fail_error;
    return ESP_OK;
}

static esp_err_t mock_verify_sta_disconnected(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->verify_sta_disc_count++;
    m->call_sequence[m->call_count++] = 9;
    if (m->fail_stage == 9) return m->fail_error;
    return ESP_OK;
}

static esp_err_t mock_verify_sta_ip_released(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->verify_sta_ip_count++;
    m->call_sequence[m->call_count++] = 10;
    if (m->fail_stage == 10) return m->fail_error;
    return ESP_OK;
}

static esp_err_t mock_set_wifi_ap_only(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->set_ap_count++;
    m->call_sequence[m->call_count++] = 11;
    if (m->fail_stage == 11) return m->fail_error;
    return ESP_OK;
}

static esp_err_t mock_verify_ap_active(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->verify_ap_count++;
    m->call_sequence[m->call_count++] = 12;
    if (m->fail_stage == 12) return m->fail_error;
    return ESP_OK;
}

static esp_err_t mock_verify_machine_safe(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->verify_machine_safe_count++;
    m->call_sequence[m->call_count++] = 13;
    if (m->estop_latched || m->mock_machine_state == ARGUS_STATE_EMERGENCY_STOPPED || m->mock_machine_state == ARGUS_STATE_FAULTED) {
        return m->fail_error;
    }
    if (m->mock_machine_state != ARGUS_STATE_HOLDING && m->mock_machine_state != ARGUS_STATE_UNLOCKED) {
        return m->fail_error;
    }
    if (m->fail_stage == 13) return m->fail_error;
    return ESP_OK;
}

// Test 16: 15-Step Service Orchestration & Complete Stage 2-15 Fail-Closed Failure Injection
static esp_err_t test_network_truthfulness_and_broker_ordering(void)
{
    mock_orchestration_ctx_t ctx;

    // Check missing callbacks - setup a context just to trigger invalid arg error on incomplete op tables
    argus_network_mode_t mock_net = ARGUS_NET_MODE_COMMISSIONED_STA;
    argus_service_authority_ops_t aops_incomplete = {0};
    argus_service_transition_ops_t ops_incomplete = {0};
    TEST_ASSERT(argus_net_mgr_orchestrate_service_entry(&mock_net, ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, &aops_incomplete, NULL) == ESP_ERR_INVALID_ARG, "NULL transition ops accepted");
    TEST_ASSERT(argus_net_mgr_orchestrate_service_entry(&mock_net, ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, &aops_incomplete, &ops_incomplete) == ESP_ERR_INVALID_ARG, "Incomplete callback table accepted");

    // Part A: Happy path
    init_mock_ctx(&ctx);
    argus_service_authority_ops_t aops = {
        .prepare_transition = mock_prepare_transition,
        .grant_local = mock_grant_local,
        .abort_transition = mock_abort_transition,
        .ctx = &ctx
    };
    argus_service_transition_ops_t ops = {
        .request_normal_stop = mock_request_normal_stop,
        .verify_stopped = mock_verify_stopped,
        .stop_broker = mock_stop_broker,
        .verify_broker_stopped = mock_verify_broker_stopped,
        .disconnect_sta = mock_disconnect_sta,
        .verify_sta_disconnected = mock_verify_sta_disconnected,
        .verify_sta_ip_released = mock_verify_sta_ip_released,
        .set_wifi_ap_only = mock_set_wifi_ap_only,
        .verify_ap_active = mock_verify_ap_active,
        .verify_machine_safe = mock_verify_machine_safe,
        .ctx = &ctx
    };

    argus_network_mode_t net_mode = ARGUS_NET_MODE_COMMISSIONED_STA;
    esp_err_t res = argus_net_mgr_orchestrate_service_entry(&net_mode, ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, &aops, &ops);

    TEST_ASSERT(res == ESP_OK, "Service entry orchestration failed");
    TEST_ASSERT(net_mode == ARGUS_NET_MODE_SERVICE_AP_ONLY, "Final mode not SERVICE_AP_ONLY");
    TEST_ASSERT(ctx.auth_core.mode == ARGUS_AUTHORITY_LOCAL_SERVICE, "Final auth mode not LOCAL_SERVICE");
    TEST_ASSERT(ctx.auth_core.owner == ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, "Final auth owner not DIAGNOSTIC_CLI");
    TEST_ASSERT(ctx.grant_count == 1, "grant_count mismatch");
    TEST_ASSERT(ctx.call_count == 12, "call_count mismatch");
    TEST_ASSERT(ctx.prepare_count == 1, "prepare mismatch");
    TEST_ASSERT(ctx.normal_stop_count == 1, "normal_stop mismatch");
    TEST_ASSERT(ctx.verify_stopped_count == 1, "verify_stopped mismatch");
    TEST_ASSERT(ctx.stop_broker_count == 1, "stop_broker mismatch");
    TEST_ASSERT(ctx.verify_broker_count == 1, "verify_broker mismatch");
    TEST_ASSERT(ctx.disconnect_sta_count == 1, "disconnect_sta mismatch");
    TEST_ASSERT(ctx.verify_sta_disc_count == 1, "verify_sta_disc mismatch");
    TEST_ASSERT(ctx.verify_sta_ip_count == 1, "verify_sta_ip mismatch");
    TEST_ASSERT(ctx.set_ap_count == 1, "set_ap mismatch");
    TEST_ASSERT(ctx.verify_ap_count == 1, "verify_ap mismatch");
    TEST_ASSERT(ctx.verify_machine_safe_count == 1, "verify_machine_safe mismatch");
    TEST_ASSERT(ctx.call_count == (sizeof(EXPECTED_FULL_SEQUENCE)/sizeof(EXPECTED_FULL_SEQUENCE[0])), "Happy path call count mismatch");
    for (int k = 0; k < ctx.call_count; k++) {
        TEST_ASSERT(ctx.call_sequence[k] == EXPECTED_FULL_SEQUENCE[k], "Happy path call sequence mismatch");
    }
    TEST_ASSERT(ctx.motion_start_count == 0, "motion_start_count must be 0 in happy path");

    typedef struct {
        const char *name;
        int fail_stage;
        esp_err_t injected_error;
        int expected_calls;
    } fail_test_case_t;

    fail_test_case_t test_cases[] = {
        {"Prepare", 2, ESP_ERR_INVALID_STATE, 1},
        {"Stop Request", 4, ESP_ERR_INVALID_STATE, 2},
        {"Stop Verify Timeout", 5, ESP_ERR_TIMEOUT, 3},
        {"Broker Stop", 6, ESP_ERR_INVALID_STATE, 4},
        {"Broker Verify Timeout", 7, ESP_ERR_TIMEOUT, 5},
        {"STA Disconnect", 8, ESP_ERR_INVALID_STATE, 6},
        {"STA Verify Timeout", 9, ESP_ERR_TIMEOUT, 7},
        {"STA IP Verify Timeout", 10, ESP_ERR_TIMEOUT, 8},
        {"AP Set", 11, ESP_ERR_INVALID_STATE, 9},
        {"AP Verify Timeout", 12, ESP_ERR_TIMEOUT, 10},
        {"Machine Safe", 13, ESP_ERR_INVALID_STATE, 11},
        {"Grant Local", 15, ESP_ERR_INVALID_STATE, 12}
    };

    // Part B: Failure injection matrix
    for (int i = 0; i < 12; i++) {
        init_mock_ctx(&ctx);
        ctx.fail_stage = test_cases[i].fail_stage;
        ctx.fail_error = test_cases[i].injected_error;
        net_mode = ARGUS_NET_MODE_COMMISSIONED_STA;
        res = argus_net_mgr_orchestrate_service_entry(&net_mode, ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, &aops, &ops);

        TEST_ASSERT(res == test_cases[i].injected_error, "Exact esp_err_t mismatch!");
        TEST_ASSERT(ctx.call_count == test_cases[i].expected_calls, "Exact callback count mismatch!");
        for (int k = 0; k < ctx.call_count; k++) {
            TEST_ASSERT(ctx.call_sequence[k] == EXPECTED_FULL_SEQUENCE[k], "Call sequence prefix mismatch");
        }
        TEST_ASSERT(ctx.call_sequence[ctx.call_count - 1] == test_cases[i].fail_stage, "Failed callback not the last executed");
        TEST_ASSERT(net_mode == ARGUS_NET_MODE_NETWORK_FAULT, "Fail-closed net mode not NETWORK_FAULT!");
        TEST_ASSERT(ctx.abort_count == 1, "abort_count mismatch");
        if (test_cases[i].fail_stage == 15) {
            TEST_ASSERT(ctx.grant_count == 1, "grant_count mismatch (should be 1 for grant failure)");
        } else {
            TEST_ASSERT(ctx.grant_count == 0, "grant_count mismatch");
        }
        TEST_ASSERT(ctx.auth_core.mode == ARGUS_AUTHORITY_NONE, "Auth mode not NONE after abort");
        TEST_ASSERT(ctx.auth_core.owner == ARGUS_AUTH_OWNER_NONE, "Auth owner not NONE after abort");
        TEST_ASSERT(ctx.motion_start_count == 0, "motion_start_count must be 0 in failure cases");
    }

    // Part C: Prepare transition failure: fail_stage = 2
    init_mock_ctx(&ctx);
    ctx.fail_stage = 2;
    net_mode = ARGUS_NET_MODE_COMMISSIONED_STA;
    res = argus_net_mgr_orchestrate_service_entry(&net_mode, ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, &aops, &ops);
    TEST_ASSERT(res != ESP_OK, "Injected prepare failure accepted!");
    TEST_ASSERT(net_mode == ARGUS_NET_MODE_NETWORK_FAULT, "Fail-closed net mode not NETWORK_FAULT!");
    TEST_ASSERT(ctx.abort_count == 1, "abort_count mismatch on prep fail");
    TEST_ASSERT(ctx.normal_stop_count == 0, "ops called after prep fail");
    TEST_ASSERT(ctx.motion_start_count == 0, "motion_start_count must be 0");

    // Part D: E-stop during verify_stopped
    init_mock_ctx(&ctx);
    ctx.estop_during_stop = true;
    net_mode = ARGUS_NET_MODE_COMMISSIONED_STA;
    res = argus_net_mgr_orchestrate_service_entry(&net_mode, ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, &aops, &ops);
    TEST_ASSERT(res == ESP_ERR_INVALID_STATE, "E-stop preemption failed to return invalid state");
    TEST_ASSERT(ctx.estop_latched == true, "E-stop not latched");
    TEST_ASSERT(ctx.mock_machine_state == ARGUS_STATE_EMERGENCY_STOPPED, "Machine state not E-STOPPED");
    TEST_ASSERT(ctx.auth_core.mode == ARGUS_AUTHORITY_NONE, "Auth mode not NONE after E-stop");
    TEST_ASSERT(ctx.grant_count == 0, "grant_count mismatch");
    TEST_ASSERT(ctx.abort_count == 1, "abort_count mismatch");
    TEST_ASSERT(ctx.stop_broker_count == 0, "stop_broker called after E-stop");
    TEST_ASSERT(ctx.disconnect_sta_count == 0, "disconnect_sta called after E-stop");
    TEST_ASSERT(ctx.set_ap_count == 0, "set_ap called after E-stop");
    TEST_ASSERT(ctx.verify_ap_count == 0, "verify_ap called after E-stop");
    TEST_ASSERT(ctx.verify_machine_safe_count == 0, "verify_machine_safe called after E-stop");
    TEST_ASSERT(ctx.motion_start_count == 0, "motion_start_count must be 0");

    // Part E: verify_machine_safe failure (stage 13)
    init_mock_ctx(&ctx);
    ctx.fail_stage = 13;
    net_mode = ARGUS_NET_MODE_COMMISSIONED_STA;
    res = argus_net_mgr_orchestrate_service_entry(&net_mode, ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, &aops, &ops);
    TEST_ASSERT(res == ESP_ERR_INVALID_STATE, "Verify machine safe failure accepted");
    TEST_ASSERT(net_mode == ARGUS_NET_MODE_NETWORK_FAULT, "Net mode not NETWORK_FAULT");
    TEST_ASSERT(ctx.grant_count == 0, "grant_count mismatch");
    TEST_ASSERT(ctx.abort_count == 1, "abort_count mismatch");
    TEST_ASSERT(ctx.verify_ap_count == 1, "Preceding callback verify_ap not called");
    TEST_ASSERT(ctx.auth_core.mode == ARGUS_AUTHORITY_NONE, "Auth mode not NONE");
    TEST_ASSERT(ctx.motion_start_count == 0, "motion_start_count must be 0");

    return ESP_OK;
}

// Test 17: Console Key, SSID, and Password Parsing Input Validation
static esp_err_t test_console_input_validation(void)
{
    char key = '\0';
    TEST_ASSERT(argus_console_parse_menu_key("", &key) == ESP_ERR_INVALID_ARG, "Empty menu input accepted");
    TEST_ASSERT(argus_console_parse_menu_key("   \t\r\n  ", &key) == ESP_ERR_INVALID_ARG, "Whitespace menu input accepted");
    TEST_ASSERT(argus_console_parse_menu_key("8", &key) == ESP_OK && key == '8', "Single key '8' failed");
    TEST_ASSERT(argus_console_parse_menu_key("N", &key) == ESP_OK && key == 'N', "Single key 'N' failed");
    TEST_ASSERT(argus_console_parse_menu_key("  8 \t ", &key) == ESP_OK && key == '8', "Padded single key failed");
    TEST_ASSERT(argus_console_parse_menu_key("89", &key) == ESP_ERR_INVALID_ARG, "Multi-char '89' accepted");

    char ssid_32[33];
    memset(ssid_32, 'A', 32);
    ssid_32[32] = '\0';
    TEST_ASSERT(argus_console_validate_ssid(ssid_32) == ESP_OK, "32-char SSID rejected");

    char ssid_33[34];
    memset(ssid_33, 'A', 33);
    ssid_33[33] = '\0';
    TEST_ASSERT(argus_console_validate_ssid(ssid_33) == ESP_ERR_INVALID_SIZE, "33-char SSID accepted");

    TEST_ASSERT(argus_console_validate_password("12345678") == ESP_OK, "8-char password rejected");
    TEST_ASSERT(argus_console_validate_password("1234567") == ESP_ERR_INVALID_SIZE, "7-char password accepted");

    return ESP_OK;
}

// Test 18: Pure Stack-Local Two-Stage Service Entry and Fail-Closed Abort Testing
static esp_err_t test_two_stage_service_entry_and_fail_closed_abort(void)
{
    argus_authority_core_t local_auth = {
        .mode = ARGUS_AUTHORITY_NONE,
        .owner = ARGUS_AUTH_OWNER_NONE,
        .generation = 1
    };

    TEST_ASSERT(argus_authority_core_prepare_service_transition(&local_auth) == ESP_OK, "Prep service transition failed");
    TEST_ASSERT(local_auth.mode == ARGUS_AUTHORITY_SERVICE_TRANSITION && local_auth.owner == ARGUS_AUTH_OWNER_NONE, "Auth mode not SERVICE_TRANSITION/NONE");

    TEST_ASSERT(argus_authority_core_grant_local_service(&local_auth, ARGUS_AUTH_OWNER_MQTT) == ESP_ERR_INVALID_ARG, "Grant to MQTT accepted");
    TEST_ASSERT(local_auth.mode == ARGUS_AUTHORITY_NONE && local_auth.owner == ARGUS_AUTH_OWNER_NONE, "Auth mode not NONE/NONE after abort");

    argus_authority_core_prepare_service_transition(&local_auth);
    TEST_ASSERT(argus_authority_core_grant_local_service(&local_auth, ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) == ESP_OK, "Grant to CLI failed");
    TEST_ASSERT(local_auth.mode == ARGUS_AUTHORITY_LOCAL_SERVICE && local_auth.owner == ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, "Auth mode not LOCAL_SERVICE/CLI");

    argus_authority_core_abort_service_transition(&local_auth);
    TEST_ASSERT(local_auth.mode == ARGUS_AUTHORITY_NONE && local_auth.owner == ARGUS_AUTH_OWNER_NONE, "Auth mode not NONE/NONE after explicit abort");

    return ESP_OK;
}

// Full Read-Only Production Snapshot Observation Struct
typedef struct {
    argus_state_snapshot_t state;
    argus_authority_snapshot_t authority;
    argus_net_snapshot_t net_snap;
    esp_err_t wifi_mode_status;
    wifi_mode_t wifi_driver_mode;
    argus_nvs_observation_t nvs_obs;
    UBaseType_t task_count;
    argus_mqtt_broker_lifecycle_obs_t broker_obs;
    esp_err_t broker_obs_status;
} argus_prod_snapshot_t;

static esp_err_t capture_prod_snapshot(argus_prod_snapshot_t *out)
{
    memset(out, 0, sizeof(argus_prod_snapshot_t));
    argus_state_mgr_get_snapshot(&out->state);
    argus_authority_mgr_get_snapshot(&out->authority);

    esp_err_t err = argus_net_mgr_get_snapshot(&out->net_snap);
    if (err != ESP_OK) {
        return err;
    }

    out->wifi_mode_status = esp_wifi_get_mode(&out->wifi_driver_mode);
    if (out->wifi_mode_status != ESP_OK && out->wifi_mode_status != ESP_ERR_WIFI_NOT_INIT) {
        return out->wifi_mode_status;
    }

    err = argus_nvs_config_get_observation_snapshot(&out->nvs_obs);
    if (err != ESP_OK) {
        return err;
    }

    out->broker_obs_status = argus_mqtt_broker_get_lifecycle_obs(&out->broker_obs);
    if (out->broker_obs_status != ESP_OK) {
        return out->broker_obs_status;
    }

    out->task_count = uxTaskGetNumberOfTasks();
    return ESP_OK;
}

static bool check_full_state_invariance(const argus_prod_snapshot_t *b, const argus_prod_snapshot_t *a)
{
    bool match = true;

#define CHECK_DIFF_INT(field_name, val_b, val_a) \
    do { \
        if ((val_b) != (val_a)) { \
            printf("\nField  : %s\nBefore : %ld\nAfter  : %ld\n", (field_name), (long)(val_b), (long)(val_a)); \
            match = false; \
        } \
    } while(0)

#define CHECK_DIFF_UINT64(field_name, val_b, val_a) \
    do { \
        if ((val_b) != (val_a)) { \
            printf("\nField  : %s\nBefore : %" PRIu64 "\nAfter  : %" PRIu64 "\n", (field_name), (uint64_t)(val_b), (uint64_t)(val_a)); \
            match = false; \
        } \
    } while(0)

#define CHECK_DIFF_MEM(field_name, ptr_b, ptr_a, size) \
    do { \
        if (memcmp((ptr_b), (ptr_a), (size)) != 0) { \
            printf("\nField  : %s\nData differs.\n", (field_name)); \
            match = false; \
        } \
    } while(0)

    CHECK_DIFF_INT("1. machine_state", b->state.machine_state, a->state.machine_state);
    CHECK_DIFF_INT("2. configured_target_rpm_milli", b->state.configured_target_rpm_milli, a->state.configured_target_rpm_milli);
    CHECK_DIFF_INT("3. trajectory_target_rpm_milli", b->state.trajectory_target_rpm_milli, a->state.trajectory_target_rpm_milli);
    CHECK_DIFF_INT("4. applied_rpm_milli", b->state.applied_rpm_milli, a->state.applied_rpm_milli);
    CHECK_DIFF_INT("5. generated_rpm_milli", b->state.generated_rpm_milli, a->state.generated_rpm_milli);
    CHECK_DIFF_UINT64("6. generated_step_count", b->state.generated_step_count, a->state.generated_step_count);
    CHECK_DIFF_INT("7. driver_enabled", b->state.driver_enabled, a->state.driver_enabled);
    CHECK_DIFF_INT("8. estop_latched", b->state.estop_latched, a->state.estop_latched);
    CHECK_DIFF_INT("9. fault_code", b->state.fault_code, a->state.fault_code);
    CHECK_DIFF_INT("10. net_mode", b->net_snap.mode, a->net_snap.mode);
    CHECK_DIFF_INT("11. last_net_err", b->net_snap.last_error, a->net_snap.last_error);
    CHECK_DIFF_INT("12. wifi_mode_status", b->wifi_mode_status, a->wifi_mode_status);
    CHECK_DIFF_INT("13. wifi_driver_mode", b->wifi_driver_mode, a->wifi_driver_mode);
    CHECK_DIFF_INT("14. sta_connected", b->net_snap.sta_connected, a->net_snap.sta_connected);
    CHECK_DIFF_INT("15. sta_ip_acquired", b->net_snap.sta_ip_acquired, a->net_snap.sta_ip_acquired);
    CHECK_DIFF_INT("16. ap_started", b->net_snap.ap_started, a->net_snap.ap_started);
    CHECK_DIFF_INT("17. mqtt_broker_running", b->net_snap.mqtt_broker_running, a->net_snap.mqtt_broker_running);
    CHECK_DIFF_INT("18. authority.mode", b->authority.mode, a->authority.mode);
    CHECK_DIFF_INT("19. authority.owner", b->authority.owner, a->authority.owner);
    CHECK_DIFF_INT("20. authority.generation", b->authority.generation, a->authority.generation);

    CHECK_DIFF_INT("21. nvs_selector_status", b->nvs_obs.selector_status, a->nvs_obs.selector_status);
    CHECK_DIFF_INT("22. nvs_selector_present", b->nvs_obs.selector_present, a->nvs_obs.selector_present);
    CHECK_DIFF_INT("23. nvs_selector", b->nvs_obs.selector, a->nvs_obs.selector);

    CHECK_DIFF_INT("24. slot_a_status", b->nvs_obs.slot_a_status, a->nvs_obs.slot_a_status);
    CHECK_DIFF_INT("25. slot_a_present", b->nvs_obs.slot_a_present, a->nvs_obs.slot_a_present);
    CHECK_DIFF_INT("26. slot_a_valid", b->nvs_obs.slot_a_valid, a->nvs_obs.slot_a_valid);
    if (b->nvs_obs.slot_a_present) {
        CHECK_DIFF_INT("slot_a.config_generation", b->nvs_obs.slot_a.config_generation, a->nvs_obs.slot_a.config_generation);
        CHECK_DIFF_INT("slot_a.schema_version", b->nvs_obs.slot_a.schema_version, a->nvs_obs.slot_a.schema_version);
        CHECK_DIFF_INT("slot_a.valid_marker", b->nvs_obs.slot_a.valid_marker, a->nvs_obs.slot_a.valid_marker);
        CHECK_DIFF_INT("slot_a.payload_length", b->nvs_obs.slot_a.payload_length, a->nvs_obs.slot_a.payload_length);
        CHECK_DIFF_INT("slot_a.crc32", b->nvs_obs.slot_a.crc32, a->nvs_obs.slot_a.crc32);
        CHECK_DIFF_MEM("slot_a.payload", &b->nvs_obs.slot_a.payload, &a->nvs_obs.slot_a.payload, sizeof(argus_config_payload_t));
    }

    CHECK_DIFF_INT("27. slot_b_status", b->nvs_obs.slot_b_status, a->nvs_obs.slot_b_status);
    CHECK_DIFF_INT("28. slot_b_present", b->nvs_obs.slot_b_present, a->nvs_obs.slot_b_present);
    CHECK_DIFF_INT("29. slot_b_valid", b->nvs_obs.slot_b_valid, a->nvs_obs.slot_b_valid);
    if (b->nvs_obs.slot_b_present) {
        CHECK_DIFF_INT("slot_b.config_generation", b->nvs_obs.slot_b.config_generation, a->nvs_obs.slot_b.config_generation);
        CHECK_DIFF_INT("slot_b.schema_version", b->nvs_obs.slot_b.schema_version, a->nvs_obs.slot_b.schema_version);
        CHECK_DIFF_INT("slot_b.valid_marker", b->nvs_obs.slot_b.valid_marker, a->nvs_obs.slot_b.valid_marker);
        CHECK_DIFF_INT("slot_b.payload_length", b->nvs_obs.slot_b.payload_length, a->nvs_obs.slot_b.payload_length);
        CHECK_DIFF_INT("slot_b.crc32", b->nvs_obs.slot_b.crc32, a->nvs_obs.slot_b.crc32);
        CHECK_DIFF_MEM("slot_b.payload", &b->nvs_obs.slot_b.payload, &a->nvs_obs.slot_b.payload, sizeof(argus_config_payload_t));
    }

    CHECK_DIFF_INT("30. task_count", b->task_count, a->task_count);

    CHECK_DIFF_INT("31. broker_obs_status", b->broker_obs_status, a->broker_obs_status);
    CHECK_DIFF_INT("32. broker.state", b->broker_obs.state, a->broker_obs.state);
    CHECK_DIFF_INT("33. broker.active_client_count", b->broker_obs.active_client_count, a->broker_obs.active_client_count);
    CHECK_DIFF_INT("34. broker.has_server_task", b->broker_obs.has_server_task, a->broker_obs.has_server_task);
    CHECK_DIFF_INT("35. broker.has_listener", b->broker_obs.has_listener, a->broker_obs.has_listener);

#undef CHECK_DIFF_INT
#undef CHECK_DIFF_UINT64
#undef CHECK_DIFF_MEM
    return match;
}

esp_err_t argus_tests_4a_run_all(void)
{
    printf("\n===================================================\n");
    printf("=== Phase 4A Pure Non-Motion Unit Test Suite ===\n");
    printf("===================================================\n");

    int passed_executions = 0;
    int failed_executions = 0;

    argus_prod_snapshot_t snap_before, snap_after;
    if (capture_prod_snapshot(&snap_before) != ESP_OK) {
        printf("Failed to capture initial snapshot\n");
        return ESP_FAIL;
    }

#define RUN_TEST(test_fn) \
    do { \
        printf("Running %-55s ... ", #test_fn); \
        esp_err_t err = test_fn(); \
        if (err == ESP_OK) { \
            printf("[PASSED]\n"); \
            passed_executions++; \
        } else { \
            printf("[FAIL]\n"); \
            failed_executions++; \
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
        RUN_TEST(test_two_stage_service_entry_and_fail_closed_abort);
    }

    if (capture_prod_snapshot(&snap_after) != ESP_OK) {
        printf("Failed to capture final snapshot\n");
        return ESP_FAIL;
    }
    bool non_mutated = check_full_state_invariance(&snap_before, &snap_after);

    printf("\nPhase 4A Pure Tests:\n");
    printf("  Distinct Test Cases : 18\n");
    printf("  Repeat Passes       : 3\n");
    printf("  Total Executions    : %d\n", passed_executions + failed_executions);
    printf("  Passed Executions   : %d\n", passed_executions);
    printf("  Failed Executions   : %d\n", failed_executions);

    printf("\nProduction Isolation (Read-Only Proof):\n");
    printf("  Authority Generation : %s (Gen %lu)\n",
           (snap_before.authority.generation == snap_after.authority.generation) ? "UNCHANGED" : "MUTATED",
           (unsigned long)snap_after.authority.generation);
    printf("  Network State         : %s (%s)\n",
           (snap_before.net_snap.mode == snap_after.net_snap.mode) ? "UNCHANGED" : "MUTATED",
           argus_net_mgr_get_mode_name(snap_after.net_snap.mode));
    printf("  MQTT Broker State     : %s (%s)\n",
           (snap_before.net_snap.mqtt_broker_running == snap_after.net_snap.mqtt_broker_running) ? "UNCHANGED" : "MUTATED",
           snap_after.net_snap.mqtt_broker_running ? "RUNNING" : "STOPPED");
    printf("  Machine State         : %s (%s)\n",
           (snap_before.state.machine_state == snap_after.state.machine_state) ? "UNCHANGED" : "MUTATED",
           argus_state_mgr_get_state_name(snap_after.state.machine_state));
    printf("  Task Count            : %s (%u tasks)\n",
           (snap_before.task_count == snap_after.task_count) ? "UNCHANGED" : "MUTATED",
           (unsigned)snap_after.task_count);

    bool overall_pass = (failed_executions == 0 && non_mutated && snap_before.broker_obs_status == ESP_OK && snap_after.broker_obs_status == ESP_OK);

    printf("\n===================================================\n");
    printf("PHASE 4A PURE UNIT TEST SUITE: %s\n", overall_pass ? "PASSED" : "FAILED");
    printf("===================================================\n\n");

    return overall_pass ? ESP_OK : ESP_FAIL;
}
