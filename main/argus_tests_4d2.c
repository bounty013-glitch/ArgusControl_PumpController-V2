#include "argus_tests_4d2.h"

#include <string.h>

#include "argus_local_recovery.h"
#include "argus_net_mgr.h"
#include "argus_password_verifier.h"
#include "argus_security_migration.h"
#include "argus_security_store.h"
#include "argus_security_provisioning.h"
#include "nvs.h"

#define CHECK(condition) do { if (!(condition)) return ESP_FAIL; } while (0)

typedef struct {
    argus_security_slot_t slots[2];
    bool slot_present[2];
    uint8_t selector;
    bool selector_present;
    esp_err_t write_slot_error;
    esp_err_t write_selector_error;
    uint32_t write_slot_calls;
    uint32_t write_selector_calls;
} security_mock_t;

static esp_err_t mock_read_slot(void *ctx, uint8_t index,
                                argus_security_slot_t *out)
{
    security_mock_t *mock = ctx;
    if (index > 1U || !mock->slot_present[index]) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = mock->slots[index];
    return ESP_OK;
}

static esp_err_t mock_write_slot(void *ctx, uint8_t index,
                                 const argus_security_slot_t *slot)
{
    security_mock_t *mock = ctx;
    mock->write_slot_calls++;
    if (mock->write_slot_error != ESP_OK) return mock->write_slot_error;
    mock->slots[index] = *slot;
    mock->slot_present[index] = true;
    return ESP_OK;
}

static esp_err_t mock_read_selector(void *ctx, uint8_t *out)
{
    security_mock_t *mock = ctx;
    if (!mock->selector_present) return ESP_ERR_NVS_NOT_FOUND;
    *out = mock->selector;
    return ESP_OK;
}

static esp_err_t mock_write_selector(void *ctx, uint8_t selector)
{
    security_mock_t *mock = ctx;
    mock->write_selector_calls++;
    if (mock->write_selector_error != ESP_OK) {
        return mock->write_selector_error;
    }
    mock->selector = selector;
    mock->selector_present = true;
    return ESP_OK;
}

static argus_security_store_driver_t mock_driver(security_mock_t *mock)
{
    return (argus_security_store_driver_t) {
        .read_slot = mock_read_slot,
        .write_slot = mock_write_slot,
        .read_selector = mock_read_selector,
        .write_selector = mock_write_selector,
        .ctx = mock,
    };
}

static void valid_verifier(argus_password_verifier_t *record, uint8_t seed)
{
    memset(record, 0, sizeof(*record));
    record->format_version = ARGUS_PASSWORD_FORMAT_VERSION;
    record->algorithm = ARGUS_PASSWORD_ALGORITHM_PBKDF2_HMAC_SHA256;
    record->salt_length = ARGUS_PASSWORD_SALT_SIZE;
    record->verifier_length = ARGUS_PASSWORD_VERIFIER_SIZE;
    record->iterations = ARGUS_PASSWORD_ITERATIONS_MIN;
    memset(record->salt, seed, sizeof(record->salt));
    memset(record->verifier, (uint8_t)(seed + 1U), sizeof(record->verifier));
}

static void count_pbkdf2_cooperation(void *ctx)
{
    uint32_t *count = (uint32_t *)ctx;
    ++(*count);
}

static void provision_ap(argus_security_ap_secret_record_t *record,
                         uint8_t seed)
{
    record->record_version = ARGUS_SECURITY_RECORD_VERSION;
    record->provisioned = 1U;
    record->length = ARGUS_SECURITY_AP_SECRET_MIN;
    record->credential_version = 1U;
    memset(record->value, seed, record->length);
    record->value[record->length] = 0U;
}

esp_err_t test_4d2_permission_ceiling_metadata(void)
{
    argus_security_payload_t payload;
    argus_security_store_default_payload(&payload);
    CHECK(payload.role_count == ARGUS_SECURITY_BUILTIN_ROLE_COUNT);
    const argus_security_role_record_t *argus =
        &payload.builtin_roles[ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL];
    const argus_security_role_record_t *client =
        &payload.builtin_roles[ARGUS_SECURITY_LEVEL_CLIENT_ADMIN];
    CHECK((argus->permissions & ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) != 0U);
    CHECK((argus->delegable_permissions &
           ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) == 0U);
    CHECK((client->permissions & ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) == 0U);
    CHECK((client->delegable_permissions &
           ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) == 0U);
    CHECK(argus->protected_role == 1U);
    return ESP_OK;
}

esp_err_t test_4d2_record_schema_validation(void)
{
    argus_password_verifier_t verifier;
    valid_verifier(&verifier, 1U);
    argus_security_human_record_t human = {
        .record_version = ARGUS_SECURITY_RECORD_VERSION,
        .level = ARGUS_SECURITY_LEVEL_OPERATOR,
        .enabled = 1U,
        .role_mask = 1U,
        .credential_version = 1U,
        .record_security_epoch = 1U,
        .verifier = verifier,
    };
    strlcpy(human.identifier, "operator_01", sizeof(human.identifier));
    strlcpy(human.login, "operator_01", sizeof(human.login));
    strlcpy(human.display_name, "Synthetic Operator",
            sizeof(human.display_name));
    strlcpy(human.scope, "synthetic_client", sizeof(human.scope));
    CHECK(argus_security_human_record_valid(&human));
    human.level = ARGUS_SECURITY_LEVEL_MACHINE;
    CHECK(!argus_security_human_record_valid(&human));
    human.level = ARGUS_SECURITY_LEVEL_OPERATOR;
    human.identifier[0] = '/';
    CHECK(!argus_security_human_record_valid(&human));

    argus_security_machine_record_t machine = {
        .record_version = ARGUS_SECURITY_RECORD_VERSION,
        .enabled = 1U,
        .client_type = ARGUS_MACHINE_CLIENT_HMI,
        .allowed_transports = ARGUS_MACHINE_TRANSPORT_MQTT,
        .permissions = ARGUS_PERMISSION_VIEW_STATUS,
        .credential_version = 1U,
        .record_security_epoch = 1U,
        .verifier = verifier,
    };
    strlcpy(machine.identifier, "synthetic_hmi", sizeof(machine.identifier));
    strlcpy(machine.display_name, "Synthetic HMI",
            sizeof(machine.display_name));
    strlcpy(machine.scope, "synthetic_client", sizeof(machine.scope));
    strlcpy(machine.topic_scope, "argus/synthetic/#",
            sizeof(machine.topic_scope));
    strlcpy(machine.enrollment_actor, "synthetic_argus_personnel",
            sizeof(machine.enrollment_actor));
    CHECK(argus_security_machine_record_valid(&machine));
    machine.permissions = UINT64_MAX;
    CHECK(!argus_security_machine_record_valid(&machine));
    return ESP_OK;
}

esp_err_t test_4d2_manifest_validation(void)
{
    argus_security_payload_t payload;
    argus_security_store_default_payload(&payload);
    CHECK(argus_security_payload_valid(&payload));
    payload.role_count = ARGUS_SECURITY_MAX_ROLES + 1U;
    CHECK(!argus_security_payload_valid(&payload));
    argus_security_store_default_payload(&payload);
    payload.recovery_state = 99U;
    CHECK(!argus_security_payload_valid(&payload));
    argus_security_store_default_payload(&payload);
    payload.console_verifier_provisioned = 1U;
    CHECK(!argus_security_payload_valid(&payload));
    argus_security_store_default_payload(&payload);
    provision_ap(&payload.factory_ap, 'f');
    provision_ap(&payload.active_ap, 'a');
    payload.active_ap.value[0] = 0x1fU;
    CHECK(!argus_security_payload_valid(&payload));
    return ESP_OK;
}

esp_err_t test_4d2_store_missing_initialization(void)
{
    security_mock_t mock = {0};
    argus_security_store_driver_t driver = mock_driver(&mock);
    argus_security_store_core_t core;
    CHECK(argus_security_store_core_init(&core, &driver) == ESP_OK);
    CHECK(core.state == ARGUS_SECURITY_STORE_MISSING);
    CHECK(core.generation == 0U);
    CHECK(argus_security_payload_valid(&core.active));
    return ESP_OK;
}

esp_err_t test_4d2_store_atomic_commit_readback(void)
{
    security_mock_t mock = {0};
    argus_security_store_driver_t driver = mock_driver(&mock);
    argus_security_store_core_t core;
    CHECK(argus_security_store_core_init(&core, &driver) == ESP_OK);
    argus_security_payload_t payload = core.active;
    provision_ap(&payload.factory_ap, 'f');
    provision_ap(&payload.active_ap, 'a');
    CHECK(argus_security_store_core_commit(&core, &payload) == ESP_OK);
    CHECK(core.generation == 1U && core.active_slot == 0U);
    CHECK(mock.write_slot_calls == 1U && mock.write_selector_calls == 1U);
    argus_security_store_core_t rebooted;
    CHECK(argus_security_store_core_init(&rebooted, &driver) == ESP_OK);
    CHECK(rebooted.state == ARGUS_SECURITY_STORE_READY);
    CHECK(rebooted.active.factory_ap.value[0] == 'f');
    CHECK(rebooted.active.active_ap.value[0] == 'a');
    return ESP_OK;
}

esp_err_t test_4d2_store_interrupted_write(void)
{
    security_mock_t mock = {0};
    argus_security_store_driver_t driver = mock_driver(&mock);
    argus_security_store_core_t core;
    CHECK(argus_security_store_core_init(&core, &driver) == ESP_OK);
    argus_security_payload_t payload = core.active;
    CHECK(argus_security_store_core_commit(&core, &payload) == ESP_OK);
    uint32_t prior = core.generation;
    mock.write_slot_error = ESP_FAIL;
    payload.security_epoch++;
    CHECK(argus_security_store_core_commit(&core, &payload) ==
          ESP_FAIL);
    CHECK(core.generation == prior);
    argus_security_store_core_t rebooted;
    CHECK(argus_security_store_core_init(&rebooted, &driver) == ESP_OK);
    CHECK(rebooted.generation == prior);
    return ESP_OK;
}

esp_err_t test_4d2_store_selector_failure(void)
{
    security_mock_t mock = {0};
    argus_security_store_driver_t driver = mock_driver(&mock);
    argus_security_store_core_t core;
    CHECK(argus_security_store_core_init(&core, &driver) == ESP_OK);
    argus_security_payload_t payload = core.active;
    CHECK(argus_security_store_core_commit(&core, &payload) == ESP_OK);
    uint32_t prior = core.generation;
    mock.write_selector_error = ESP_FAIL;
    payload.security_epoch++;
    CHECK(argus_security_store_core_commit(&core, &payload) ==
          ESP_FAIL);
    CHECK(core.generation == prior);
    argus_security_store_core_t rebooted;
    CHECK(argus_security_store_core_init(&rebooted, &driver) == ESP_OK);
    CHECK(rebooted.generation == prior);
    return ESP_OK;
}

esp_err_t test_4d2_store_initial_selector_failure(void)
{
    security_mock_t mock = {0};
    argus_security_store_driver_t driver = mock_driver(&mock);
    argus_security_store_core_t core;
    CHECK(argus_security_store_core_init(&core, &driver) == ESP_OK);
    argus_security_payload_t payload = core.active;
    mock.write_selector_error = ESP_FAIL;
    CHECK(argus_security_store_core_commit(&core, &payload) == ESP_FAIL);
    CHECK(core.generation == 0U && mock.slot_present[0] &&
          !mock.selector_present);
    argus_security_store_core_t rebooted;
    CHECK(argus_security_store_core_init(&rebooted, &driver) == ESP_OK);
    CHECK(rebooted.state == ARGUS_SECURITY_STORE_CORRUPT);
    CHECK(argus_security_store_core_commit(&rebooted, &payload) ==
          ESP_ERR_INVALID_STATE);
    return ESP_OK;
}

esp_err_t test_4d2_store_corrupt_fallback(void)
{
    security_mock_t mock = {0};
    argus_security_store_driver_t driver = mock_driver(&mock);
    argus_security_store_core_t core;
    CHECK(argus_security_store_core_init(&core, &driver) == ESP_OK);
    argus_security_payload_t payload = core.active;
    CHECK(argus_security_store_core_commit(&core, &payload) == ESP_OK);
    payload.security_epoch++;
    CHECK(argus_security_store_core_commit(&core, &payload) == ESP_OK);
    mock.slots[mock.selector].crc32 ^= 1U;
    argus_security_store_core_t rebooted;
    CHECK(argus_security_store_core_init(&rebooted, &driver) == ESP_OK);
    CHECK(rebooted.generation == 1U);
    CHECK(rebooted.redundancy_degraded);
    return ESP_OK;
}

esp_err_t test_4d2_store_unsupported_version(void)
{
    security_mock_t mock = {0};
    mock.slot_present[0] = true;
    mock.slots[0].magic = ARGUS_SECURITY_SLOT_MAGIC;
    mock.slots[0].schema_version = ARGUS_SECURITY_SCHEMA_VERSION + 1U;
    argus_security_store_driver_t driver = mock_driver(&mock);
    argus_security_store_core_t core;
    CHECK(argus_security_store_core_init(&core, &driver) == ESP_OK);
    CHECK(core.state == ARGUS_SECURITY_STORE_UNSUPPORTED_VERSION);
    CHECK(argus_security_store_core_commit(&core, &core.active) ==
          ESP_ERR_INVALID_STATE);
    return ESP_OK;
}

esp_err_t test_4d2_pbkdf2_known_answer(void)
{
    static const uint8_t expected[ARGUS_PASSWORD_VERIFIER_SIZE] = {
        0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c,
        0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
        0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48,
        0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b,
    };
    uint8_t actual[ARGUS_PASSWORD_VERIFIER_SIZE] = {0};
    CHECK(argus_password_pbkdf2_for_test(
              (const uint8_t *)"password", 8U,
              (const uint8_t *)"salt", 4U, 1U, actual) == ESP_OK);
    CHECK(memcmp(actual, expected, sizeof(expected)) == 0);
    argus_password_zeroize(actual, sizeof(actual));

    static const uint8_t cooperative_expected[ARGUS_PASSWORD_VERIFIER_SIZE] = {
        0x4d, 0x2e, 0x0f, 0x7c, 0x20, 0x26, 0xa0, 0xa6,
        0x15, 0x9e, 0x4e, 0x09, 0x16, 0xb7, 0xbd, 0x32,
        0x8b, 0x90, 0x43, 0x97, 0x46, 0x7a, 0x29, 0x0f,
        0xfa, 0x6b, 0x41, 0x48, 0x34, 0xb1, 0x8a, 0x2f,
    };
    uint32_t cooperation_count = 0U;
    CHECK(argus_password_pbkdf2_cooperative_for_test(
              (const uint8_t *)"password", 8U,
              (const uint8_t *)"salt", 4U, 513U, actual,
              count_pbkdf2_cooperation, &cooperation_count) == ESP_OK);
    CHECK(memcmp(actual, cooperative_expected,
                 sizeof(cooperative_expected)) == 0);
    CHECK(cooperation_count == 2U);
    argus_password_zeroize(actual, sizeof(actual));
    return ESP_OK;
}

esp_err_t test_4d2_verifier_create_verify(void)
{
    static const uint8_t password[] = "synthetic-phase4d2-password";
    argus_password_verifier_t record;
    CHECK(argus_password_verifier_create(
              password, sizeof(password) - 1U,
              ARGUS_PASSWORD_ITERATIONS_MIN, &record) == ESP_OK);
    bool match = false;
    CHECK(argus_password_verifier_verify(
              password, sizeof(password) - 1U, &record, &match) == ESP_OK);
    CHECK(match);
    static const uint8_t wrong[] = "synthetic-wrong-password";
    CHECK(argus_password_verifier_verify(
              wrong, sizeof(wrong) - 1U, &record, &match) == ESP_OK);
    CHECK(!match);
    argus_password_zeroize(&record, sizeof(record));
    return ESP_OK;
}

esp_err_t test_4d2_verifier_salt_uniqueness(void)
{
    static const uint8_t password[] = "synthetic-same-password";
    argus_password_verifier_t first;
    argus_password_verifier_t second;
    CHECK(argus_password_verifier_create(
              password, sizeof(password) - 1U,
              ARGUS_PASSWORD_ITERATIONS_MIN, &first) == ESP_OK);
    CHECK(argus_password_verifier_create(
              password, sizeof(password) - 1U,
              ARGUS_PASSWORD_ITERATIONS_MIN, &second) == ESP_OK);
    CHECK(memcmp(first.salt, second.salt, sizeof(first.salt)) != 0);
    CHECK(memcmp(first.verifier, second.verifier,
                 sizeof(first.verifier)) != 0);
    argus_password_zeroize(&first, sizeof(first));
    argus_password_zeroize(&second, sizeof(second));
    return ESP_OK;
}

esp_err_t test_4d2_verifier_malformed_records(void)
{
    argus_password_verifier_t record;
    valid_verifier(&record, 4U);
    CHECK(argus_password_verifier_record_valid(&record));
    record.algorithm = 99U;
    CHECK(!argus_password_verifier_record_valid(&record));
    valid_verifier(&record, 4U);
    record.iterations = ARGUS_PASSWORD_ITERATIONS_MIN - 1U;
    CHECK(!argus_password_verifier_record_valid(&record));
    bool match = true;
    CHECK(argus_password_verifier_verify(
              (const uint8_t *)"x", 1U, &record, &match) ==
          ESP_ERR_INVALID_ARG);
    CHECK(argus_password_verifier_create(
              (const uint8_t *)"", 0U,
              ARGUS_PASSWORD_ITERATIONS_MIN, &record) ==
          ESP_ERR_INVALID_ARG);
    uint8_t oversized[ARGUS_PASSWORD_INPUT_MAX + 1U] = {0};
    CHECK(argus_password_verifier_create(
              oversized, sizeof(oversized),
              ARGUS_PASSWORD_ITERATIONS_MIN, &record) ==
          ESP_ERR_INVALID_ARG);
    return ESP_OK;
}

static void detector_release(argus_recovery_detector_t *detector)
{
    for (uint32_t elapsed = 0U; elapsed < ARGUS_RECOVERY_DEBOUNCE_MS;
         elapsed += ARGUS_RECOVERY_SAMPLE_MS) {
        (void)argus_recovery_detector_update(
            detector, true, ARGUS_RECOVERY_SAMPLE_MS);
    }
}

esp_err_t test_4d2_recovery_startup_low_and_short_press(void)
{
    argus_recovery_detector_t detector;
    argus_recovery_detector_init(&detector, false);
    for (uint32_t elapsed = 0U; elapsed < ARGUS_RECOVERY_HOLD_MS + 1000U;
         elapsed += ARGUS_RECOVERY_SAMPLE_MS) {
        CHECK(argus_recovery_detector_update(
                  &detector, false, ARGUS_RECOVERY_SAMPLE_MS) ==
              ARGUS_RECOVERY_DETECT_NONE);
    }
    detector_release(&detector);
    CHECK(detector.startup_release_seen && !detector.triggered);
    for (uint32_t elapsed = 0U; elapsed < 1000U;
         elapsed += ARGUS_RECOVERY_SAMPLE_MS) {
        CHECK(argus_recovery_detector_update(
                  &detector, false, ARGUS_RECOVERY_SAMPLE_MS) ==
              ARGUS_RECOVERY_DETECT_NONE);
    }
    detector_release(&detector);
    CHECK(!detector.triggered);
    return ESP_OK;
}

esp_err_t test_4d2_recovery_bounce_rejection(void)
{
    argus_recovery_detector_t detector;
    argus_recovery_detector_init(&detector, true);
    detector_release(&detector);
    for (uint32_t i = 0U; i < 50U; ++i) {
        CHECK(argus_recovery_detector_update(
                  &detector, (i & 1U) != 0U,
                  ARGUS_RECOVERY_SAMPLE_MS) ==
              ARGUS_RECOVERY_DETECT_NONE);
    }
    detector_release(&detector);
    CHECK(!detector.hold_qualified && !detector.triggered);
    return ESP_OK;
}

esp_err_t test_4d2_recovery_long_hold_release_once(void)
{
    argus_recovery_detector_t detector;
    argus_recovery_detector_init(&detector, true);
    detector_release(&detector);
    for (uint32_t elapsed = 0U;
         elapsed < ARGUS_RECOVERY_HOLD_MS + ARGUS_RECOVERY_DEBOUNCE_MS;
         elapsed += ARGUS_RECOVERY_SAMPLE_MS) {
        CHECK(argus_recovery_detector_update(
                  &detector, false, ARGUS_RECOVERY_SAMPLE_MS) ==
              ARGUS_RECOVERY_DETECT_NONE);
    }
    CHECK(detector.hold_qualified && !detector.triggered);
    argus_recovery_detector_result_t result = ARGUS_RECOVERY_DETECT_NONE;
    for (uint32_t elapsed = 0U; elapsed < ARGUS_RECOVERY_DEBOUNCE_MS;
         elapsed += ARGUS_RECOVERY_SAMPLE_MS) {
        result = argus_recovery_detector_update(
            &detector, true, ARGUS_RECOVERY_SAMPLE_MS);
    }
    CHECK(result == ARGUS_RECOVERY_DETECT_QUALIFIED_RELEASE);
    CHECK(detector.triggered);
    CHECK(argus_recovery_detector_update(
              &detector, true, ARGUS_RECOVERY_SAMPLE_MS) ==
          ARGUS_RECOVERY_DETECT_NONE);
    return ESP_OK;
}

typedef struct {
    uint32_t order;
    uint32_t persisted_at;
    uint32_t posted_at;
    esp_err_t persist_error;
    esp_err_t post_error;
} recovery_ops_mock_t;

static esp_err_t recovery_persist(void *ctx)
{
    recovery_ops_mock_t *mock = ctx;
    mock->persisted_at = ++mock->order;
    return mock->persist_error;
}

static esp_err_t recovery_post(void *ctx)
{
    recovery_ops_mock_t *mock = ctx;
    mock->posted_at = ++mock->order;
    return mock->post_error;
}

esp_err_t test_4d2_recovery_commit_order_and_failures(void)
{
    CHECK(argus_net_security_recovery_request_is_idempotent(
              ARGUS_NET_MODE_SECURITY_RECOVERY_AP_ONLY));
    CHECK(!argus_net_security_recovery_request_is_idempotent(
              ARGUS_NET_MODE_SECURITY_RECOVERY_TRANSITION));
    CHECK(!argus_net_security_recovery_request_is_idempotent(
              ARGUS_NET_MODE_NETWORK_FAULT));
    CHECK(argus_net_decide_sta_event(
              ARGUS_NET_MODE_SECURITY_RECOVERY_TRANSITION,
              ARGUS_STA_EVENT_ASSOCIATED, 0U, 0U, true, true, false) ==
          ARGUS_STA_EVENT_IGNORE);
    CHECK(argus_net_decide_sta_event(
              ARGUS_NET_MODE_SECURITY_RECOVERY_AP_ONLY,
              ARGUS_STA_EVENT_DISCONNECTED, 0U, 0U, false, false, false) ==
          ARGUS_STA_EVENT_CONFIRM_DISABLED);
    recovery_ops_mock_t mock = {0};
    argus_local_recovery_ops_t ops = {
        .persist_request = recovery_persist,
        .post_network_request = recovery_post,
        .ctx = &mock,
    };
    argus_local_recovery_commit_result_t result =
        argus_local_recovery_commit(&ops);
    CHECK(result.accepted && result.persisted && result.network_posted);
    CHECK(mock.persisted_at == 1U && mock.posted_at == 2U);

    memset(&mock, 0, sizeof(mock));
    mock.persist_error = ESP_FAIL;
    result = argus_local_recovery_commit(&ops);
    CHECK(!result.accepted && !result.persisted && !result.network_posted);
    CHECK(mock.posted_at == 0U);

    memset(&mock, 0, sizeof(mock));
    mock.post_error = ESP_ERR_NO_MEM;
    result = argus_local_recovery_commit(&ops);
    CHECK(!result.accepted && result.persisted && !result.network_posted);
    CHECK(mock.persisted_at == 1U && mock.posted_at == 2U);
    return ESP_OK;
}

esp_err_t test_4d2_capacity_and_domain_bounds(void)
{
    CHECK(sizeof(argus_security_payload_t) <= 2048U);
    CHECK(sizeof(argus_security_slot_t) < 4096U);
    CHECK(ARGUS_SECURITY_MAX_HUMANS == 16U);
    CHECK(ARGUS_SECURITY_MAX_ROLES == 16U);
    CHECK(ARGUS_SECURITY_MAX_MACHINES == 16U);
    argus_security_payload_t payload;
    argus_security_store_default_payload(&payload);
    provision_ap(&payload.factory_ap, 'f');
    provision_ap(&payload.active_ap, 'a');
    CHECK(memcmp(payload.factory_ap.value, payload.active_ap.value,
                 ARGUS_SECURITY_AP_SECRET_MIN) != 0);
    payload.migration_state = ARGUS_SECURITY_MIGRATION_COMPLETE;
    payload.recovery_state = ARGUS_SECURITY_RECOVERY_REQUESTED;
    CHECK(argus_security_payload_valid(&payload));
    return ESP_OK;
}

typedef struct {
    argus_password_verifier_t verifier;
    uint32_t derive_calls;
    uint32_t write_calls;
    uint32_t read_calls;
    uint32_t verify_calls;
    esp_err_t write_error;
} provisioning_mock_t;

static esp_err_t provision_derive(void *ctx, const uint8_t *password,
                                  size_t password_len, uint32_t iterations,
                                  argus_password_verifier_t *out)
{
    provisioning_mock_t *mock = ctx;
    mock->derive_calls++;
    if (password == NULL || password_len == 0U ||
        iterations < ARGUS_PASSWORD_ITERATIONS_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    valid_verifier(out, 9U);
    mock->verifier = *out;
    return ESP_OK;
}

static esp_err_t provision_write(void *ctx, const uint8_t *factory_ap,
                                 size_t factory_ap_len,
                                 const uint8_t *active_ap,
                                 size_t active_ap_len,
                                 const argus_password_verifier_t *verifier)
{
    provisioning_mock_t *mock = ctx;
    mock->write_calls++;
    if (factory_ap == NULL || active_ap == NULL ||
        factory_ap_len < ARGUS_SECURITY_AP_SECRET_MIN ||
        active_ap_len < ARGUS_SECURITY_AP_SECRET_MIN ||
        !argus_password_verifier_record_valid(verifier)) {
        return ESP_ERR_INVALID_ARG;
    }
    return mock->write_error;
}

static esp_err_t provision_read(void *ctx,
                                argus_password_verifier_t *out)
{
    provisioning_mock_t *mock = ctx;
    mock->read_calls++;
    *out = mock->verifier;
    return ESP_OK;
}

static esp_err_t provision_verify(void *ctx, const uint8_t *password,
                                  size_t password_len,
                                  const argus_password_verifier_t *record,
                                  bool *out_match)
{
    provisioning_mock_t *mock = ctx;
    mock->verify_calls++;
    *out_match = password != NULL && password_len > 0U &&
                 argus_password_verifier_record_valid(record);
    return ESP_OK;
}

static esp_err_t provision_status(void *ctx,
                                  argus_security_store_status_t *out)
{
    (void)ctx;
    *out = (argus_security_store_status_t) {
        .schema_version = ARGUS_SECURITY_SCHEMA_VERSION,
        .security_epoch = 2U,
        .factory_ap_provisioned = true,
        .active_ap_provisioned = true,
        .console_verifier_provisioned = true,
    };
    return ESP_OK;
}

static argus_security_provisioning_ops_t provisioning_ops(
    provisioning_mock_t *mock)
{
    return (argus_security_provisioning_ops_t) {
        .derive = provision_derive,
        .write_initial = provision_write,
        .read_verifier = provision_read,
        .verify = provision_verify,
        .read_status = provision_status,
        .ctx = mock,
    };
}

esp_err_t test_4d2_provisioning_synthetic_success(void)
{
    static const uint8_t factory[] = "synthetic-factory-ap";
    static const uint8_t active[] = "synthetic-active-ap";
    static const uint8_t console[] = "synthetic-console-password";
    provisioning_mock_t mock = {0};
    argus_security_provisioning_ops_t ops = provisioning_ops(&mock);
    argus_security_provisioning_request_t request = {
        .environment = ARGUS_PROVISIONING_DEVELOPMENT,
        .factory_ap = factory,
        .factory_ap_len = sizeof(factory) - 1U,
        .active_ap = active,
        .active_ap_len = sizeof(active) - 1U,
        .console_password = console,
        .console_password_len = sizeof(console) - 1U,
        .verifier_iterations = ARGUS_PASSWORD_ITERATIONS_MIN,
        .explicit_initialization = true,
    };
    argus_security_provisioning_result_t result =
        argus_security_provisioning_execute(&request, &ops);
    CHECK(result.accepted && result.verifier_created &&
          result.record_written && result.readback_verified);
    CHECK(result.schema_version == ARGUS_SECURITY_SCHEMA_VERSION);
    CHECK(mock.derive_calls == 1U && mock.write_calls == 1U &&
          mock.read_calls == 1U && mock.verify_calls == 1U);
    return ESP_OK;
}

esp_err_t test_4d2_provisioning_rejections(void)
{
    static const uint8_t synthetic[] = "synthetic-value";
    provisioning_mock_t mock = {0};
    argus_security_provisioning_ops_t ops = provisioning_ops(&mock);
    argus_security_provisioning_request_t request = {
        .environment = ARGUS_PROVISIONING_PRODUCTION,
        .factory_ap = synthetic,
        .factory_ap_len = sizeof(synthetic) - 1U,
        .active_ap = synthetic,
        .active_ap_len = sizeof(synthetic) - 1U,
        .console_password = synthetic,
        .console_password_len = sizeof(synthetic) - 1U,
        .verifier_iterations = ARGUS_PASSWORD_ITERATIONS_MIN,
        .explicit_initialization = false,
    };
    argus_security_provisioning_result_t result =
        argus_security_provisioning_execute(&request, &ops);
    CHECK(!result.accepted && result.error == ESP_ERR_INVALID_ARG);
    CHECK(mock.derive_calls == 0U && mock.write_calls == 0U);

    request.explicit_initialization = true;
    mock.write_error = ESP_ERR_NOT_SUPPORTED;
    result = argus_security_provisioning_execute(&request, &ops);
    CHECK(!result.accepted && result.verifier_created &&
          !result.record_written && result.error == ESP_ERR_NOT_SUPPORTED);
    CHECK(mock.read_calls == 0U && mock.verify_calls == 0U);
    return ESP_OK;
}

typedef struct {
    bool marker;
    bool plaintext;
    bool verifier_present;
    bool malformed;
    esp_err_t delete_error;
    argus_password_verifier_t verifier;
    argus_security_migration_state_t state;
    uint32_t create_calls;
    uint32_t set_calls;
    uint32_t delete_calls;
} migration_mock_t;

static esp_err_t migration_inspect(void *ctx, uint8_t *password,
                                   size_t password_size, bool *marker,
                                   bool *plaintext)
{
    migration_mock_t *mock = ctx;
    if (mock->malformed) return ESP_ERR_INVALID_STATE;
    *marker = mock->marker;
    *plaintext = mock->plaintext;
    if (mock->plaintext) {
        static const char synthetic[] = "synthetic-legacy-password";
        if (password_size < sizeof(synthetic)) return ESP_ERR_INVALID_SIZE;
        memcpy(password, synthetic, sizeof(synthetic));
    }
    return ESP_OK;
}

static esp_err_t migration_get(void *ctx,
                               argus_password_verifier_t *out)
{
    migration_mock_t *mock = ctx;
    if (!mock->verifier_present) return ESP_ERR_NOT_FOUND;
    *out = mock->verifier;
    return ESP_OK;
}

static esp_err_t migration_create(void *ctx, const uint8_t *password,
                                  size_t password_len,
                                  argus_password_verifier_t *out)
{
    migration_mock_t *mock = ctx;
    if (password == NULL || password_len == 0U) return ESP_ERR_INVALID_ARG;
    mock->create_calls++;
    valid_verifier(out, 0x51U);
    return ESP_OK;
}

static esp_err_t migration_set(void *ctx,
                               const argus_password_verifier_t *record)
{
    migration_mock_t *mock = ctx;
    if (!argus_password_verifier_record_valid(record)) {
        return ESP_ERR_INVALID_ARG;
    }
    mock->set_calls++;
    mock->verifier = *record;
    mock->verifier_present = true;
    return ESP_OK;
}

static esp_err_t migration_verify(void *ctx, const uint8_t *password,
                                  size_t password_len,
                                  const argus_password_verifier_t *record,
                                  bool *out_match)
{
    (void)ctx;
    *out_match = password != NULL && password_len > 0U &&
                 argus_password_verifier_record_valid(record);
    return ESP_OK;
}

static esp_err_t migration_delete(void *ctx)
{
    migration_mock_t *mock = ctx;
    mock->delete_calls++;
    if (mock->delete_error != ESP_OK) return mock->delete_error;
    mock->marker = false;
    mock->plaintext = false;
    return ESP_OK;
}

static esp_err_t migration_state(void *ctx,
                                 argus_security_migration_state_t state)
{
    migration_mock_t *mock = ctx;
    mock->state = state;
    return ESP_OK;
}

static argus_security_migration_ops_t migration_ops(
    migration_mock_t *mock)
{
    return (argus_security_migration_ops_t) {
        .inspect_legacy = migration_inspect,
        .get_verifier = migration_get,
        .create_verifier = migration_create,
        .set_verifier = migration_set,
        .verify = migration_verify,
        .delete_legacy = migration_delete,
        .set_state = migration_state,
        .ctx = mock,
    };
}

esp_err_t test_4d2_migration_power_loss_idempotence(void)
{
    migration_mock_t mock = {
        .marker = true,
        .plaintext = true,
        .delete_error = ESP_FAIL,
    };
    argus_security_migration_ops_t ops = migration_ops(&mock);
    argus_security_migration_status_t status;
    CHECK(argus_security_migration_execute(&ops, &status) == ESP_FAIL);
    CHECK(mock.verifier_present && mock.plaintext &&
          mock.create_calls == 1U && mock.set_calls == 1U &&
          mock.state == ARGUS_SECURITY_MIGRATION_FAILED);

    mock.delete_error = ESP_OK;
    CHECK(argus_security_migration_execute(&ops, &status) == ESP_OK);
    CHECK(status.legacy_plaintext_deleted && !mock.plaintext &&
          mock.create_calls == 1U && mock.set_calls == 1U &&
          mock.state == ARGUS_SECURITY_MIGRATION_COMPLETE);

    uint32_t delete_calls = mock.delete_calls;
    CHECK(argus_security_migration_execute(&ops, &status) == ESP_OK);
    CHECK(!status.legacy_plaintext_deleted &&
          mock.delete_calls == delete_calls &&
          mock.create_calls == 1U && mock.set_calls == 1U);
    return ESP_OK;
}

esp_err_t test_4d2_migration_deferred_and_malformed(void)
{
    migration_mock_t mock = {0};
    argus_security_migration_ops_t ops = migration_ops(&mock);
    argus_security_migration_status_t status;
    CHECK(argus_security_migration_execute(&ops, &status) == ESP_OK);
    CHECK(status.build_default_deferred &&
          mock.state == ARGUS_SECURITY_MIGRATION_BUILD_DEFAULT_DEFERRED &&
          mock.create_calls == 0U && mock.delete_calls == 0U);
    mock.malformed = true;
    CHECK(argus_security_migration_execute(&ops, &status) ==
          ESP_ERR_INVALID_STATE);
    CHECK(mock.state == ARGUS_SECURITY_MIGRATION_FAILED &&
          mock.create_calls == 0U && mock.delete_calls == 0U);
    return ESP_OK;
}
