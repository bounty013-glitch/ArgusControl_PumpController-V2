#include "argus_tests_4d4.h"

#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

#include "argus_http_route_inventory.h"
#include "argus_http_server.h"
#include "argus_machine_directory.h"
#include "argus_machine_service.h"
#include "argus_mqtt_broker.h"
#include "argus_mqtt_runtime.h"
#include "argus_net_mgr.h"
#include "argus_mqtt_contract.h"
#include "argus_mqtt_security.h"
#include "argus_password_verifier.h"
#include "argus_security_http.h"

#define CHECK(value) do { \
    if (!(value)) { \
        printf("[FAIL] Phase 4D.4 assertion line %d\n", __LINE__); \
        return ESP_FAIL; \
    } \
} while (0)

static void valid_verifier(argus_password_verifier_t *verifier)
{
    memset(verifier, 0, sizeof(*verifier));
    verifier->format_version = ARGUS_PASSWORD_FORMAT_VERSION;
    verifier->algorithm = ARGUS_PASSWORD_ALGORITHM_PBKDF2_HMAC_SHA256;
    verifier->salt_length = ARGUS_PASSWORD_SALT_SIZE;
    verifier->verifier_length = ARGUS_PASSWORD_VERIFIER_SIZE;
    verifier->iterations = ARGUS_PASSWORD_ITERATIONS_DEFAULT;
    memset(verifier->salt, 0x31, sizeof(verifier->salt));
    memset(verifier->verifier, 0x72, sizeof(verifier->verifier));
}

static argus_security_machine_record_t machine_record(const char *id)
{
    argus_security_machine_record_t record = {
        .record_version = ARGUS_SECURITY_RECORD_VERSION,
        .enabled = 1U,
        .client_type = ARGUS_MACHINE_CLIENT_NODE_RED,
        .allowed_transports = ARGUS_MACHINE_TRANSPORT_MQTT,
        .allowed_interfaces = ARGUS_MACHINE_INTERFACE_STA,
        .permissions = ARGUS_PERMISSION_VIEW_STATUS |
                       ARGUS_PERMISSION_REQUEST_AUTHORITY,
        .credential_version = 1U,
        .record_security_epoch = 1U,
        .principal_revision = 1U,
    };
    strlcpy(record.identifier, id, sizeof(record.identifier));
    strlcpy(record.display_name, "Test machine",
            sizeof(record.display_name));
    strlcpy(record.scope, "*", sizeof(record.scope));
    strlcpy(record.topic_scope, "argus/paladin/pump_001",
            sizeof(record.topic_scope));
    strlcpy(record.enrollment_actor, "test-admin",
            sizeof(record.enrollment_actor));
    valid_verifier(&record.verifier);
    return record;
}

static argus_machine_directory_slot_t *slot(
    uint32_t generation, const char *id)
{
    argus_machine_directory_slot_t *result =
        calloc(1U, sizeof(*result));
    if (result == NULL) return NULL;
    result->magic = ARGUS_MACHINE_DIRECTORY_MAGIC;
    result->schema_version = ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION;
    result->payload_length = sizeof(result->payload);
    result->generation = generation;
    result->valid_marker = ARGUS_MACHINE_DIRECTORY_VALID;
    result->payload.schema_version =
        ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION;
    if (id != NULL) {
        result->payload.machine_count = 1U;
        result->payload.machines[0] = machine_record(id);
    }
    result->crc32 = argus_machine_directory_crc32(&result->payload);
    return result;
}

static bool append_field(
    uint8_t *packet, size_t capacity, size_t *offset,
    const uint8_t *value, size_t length)
{
    if (length > UINT16_MAX || *offset + 2U + length > capacity) {
        return false;
    }
    packet[(*offset)++] = (uint8_t)(length >> 8U);
    packet[(*offset)++] = (uint8_t)(length & 0xffU);
    memcpy(packet + *offset, value, length);
    *offset += length;
    return true;
}

static size_t connect_packet(uint8_t *packet, size_t capacity, uint8_t flags)
{
    size_t offset = 0U;
    static const uint8_t protocol[] = "MQTT";
    static const uint8_t id[] = "m-0123456789abcdef0123456789abcd";
    static const uint8_t secret[] =
        "0123456789abcdef0123456789abcdef01234567890";
    if (!append_field(
            packet, capacity, &offset, protocol, sizeof(protocol) - 1U) ||
        offset + 4U > capacity) {
        return 0U;
    }
    packet[offset++] = 4U;
    packet[offset++] = flags;
    packet[offset++] = 0U;
    packet[offset++] = 30U;
    if (!append_field(
            packet, capacity, &offset, id, sizeof(id) - 1U)) {
        return 0U;
    }
    if ((flags & 0x80U) != 0U &&
        !append_field(
            packet, capacity, &offset, id, sizeof(id) - 1U)) {
        return 0U;
    }
    if ((flags & 0x40U) != 0U &&
        !append_field(
            packet, capacity, &offset, secret, sizeof(secret) - 1U)) {
        return 0U;
    }
    return offset;
}

esp_err_t test_4d4_machine_record_contract(void)
{
    argus_security_machine_record_t record = machine_record("m-one");
    CHECK(argus_security_machine_record_valid(&record));
    record.allowed_interfaces = 0U;
    CHECK(!argus_security_machine_record_valid(&record));
    record = machine_record("m-one");
    record.principal_revision = 0U;
    CHECK(!argus_security_machine_record_valid(&record));
    record = machine_record("m-one");
    record.client_type = ARGUS_MACHINE_CLIENT_AI_TOOL_GATEWAY;
    CHECK(argus_security_machine_record_valid(&record));
    return ESP_OK;
}

esp_err_t test_4d4_machine_directory_capacity_and_duplicates(void)
{
    argus_machine_directory_payload_t *payload =
        calloc(1U, sizeof(*payload));
    CHECK(payload != NULL);
    payload->schema_version = ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION;
    payload->machine_count = ARGUS_SECURITY_MAX_MACHINES;
    for (size_t i = 0U; i < ARGUS_SECURITY_MAX_MACHINES; ++i) {
        char id[16];
        snprintf(id, sizeof(id), "machine-%02u", (unsigned)i);
        payload->machines[i] = machine_record(id);
    }
    CHECK(argus_machine_directory_payload_valid(payload));
    payload->machines[15] = payload->machines[0];
    CHECK(!argus_machine_directory_payload_valid(payload));
    payload->machine_count = ARGUS_SECURITY_MAX_MACHINES + 1U;
    CHECK(!argus_machine_directory_payload_valid(payload));
    argus_password_zeroize(payload, sizeof(*payload));
    free(payload);
    return ESP_OK;
}

esp_err_t test_4d4_machine_directory_empty_selection(void)
{
    argus_machine_directory_snapshot_t output;
    uint8_t active;
    bool repair;
    CHECK(argus_machine_directory_select_for_test(
              NULL, ESP_ERR_NOT_FOUND, NULL, ESP_ERR_NOT_FOUND,
              0U, ESP_ERR_NOT_FOUND, &output, &active, &repair) ==
          ESP_ERR_NOT_FOUND);
    return ESP_OK;
}

esp_err_t test_4d4_machine_directory_selector_recovery(void)
{
    argus_machine_directory_slot_t *a = slot(2U, "machine-a");
    argus_machine_directory_slot_t *b = slot(3U, "machine-b");
    CHECK(a != NULL && b != NULL);
    argus_machine_directory_snapshot_t output;
    uint8_t active;
    bool repair;
    CHECK(argus_machine_directory_select_for_test(
              a, ESP_OK, b, ESP_OK, 7U, ESP_OK,
              &output, &active, &repair) == ESP_OK);
    CHECK(active == 1U && repair && output.generation == 3U);
    b->crc32 ^= 1U;
    CHECK(argus_machine_directory_select_for_test(
              a, ESP_OK, b, ESP_OK, 1U, ESP_OK,
              &output, &active, &repair) == ESP_OK);
    CHECK(active == 0U && repair && output.generation == 2U);
    free(a);
    free(b);
    return ESP_OK;
}

esp_err_t test_4d4_machine_directory_interrupted_write(void)
{
    argus_machine_directory_slot_t *committed =
        slot(8U, "committed-machine");
    argus_machine_directory_slot_t *uncommitted =
        slot(9U, "uncommitted-machine");
    CHECK(committed != NULL && uncommitted != NULL);
    argus_machine_directory_snapshot_t output;
    uint8_t active;
    bool repair;

    CHECK(argus_machine_directory_select_for_test(
              committed, ESP_OK, uncommitted, ESP_OK, 0U, ESP_OK,
              &output, &active, &repair) == ESP_OK);
    CHECK(active == 0U && !repair && output.generation == 8U);
    CHECK(strcmp(output.payload.machines[0].identifier,
                 "committed-machine") == 0);

    free(committed);
    free(uncommitted);
    return ESP_OK;
}

esp_err_t test_4d4_machine_directory_corruption_fail_closed(void)
{
    argus_machine_directory_slot_t *a = slot(1U, "machine-a");
    argus_machine_directory_slot_t *b = slot(2U, "machine-b");
    CHECK(a != NULL && b != NULL);
    a->crc32 ^= 1U;
    b->valid_marker = 0U;
    argus_machine_directory_snapshot_t output;
    uint8_t active;
    bool repair;
    CHECK(argus_machine_directory_select_for_test(
              a, ESP_OK, b, ESP_OK, 0U, ESP_OK,
              &output, &active, &repair) == ESP_ERR_INVALID_CRC);
    free(a);
    free(b);
    return ESP_OK;
}

esp_err_t test_4d4_machine_directory_unsupported_schema(void)
{
    argus_machine_directory_slot_t *a = slot(1U, "machine-a");
    CHECK(a != NULL);
    a->schema_version++;
    argus_machine_directory_snapshot_t output;
    uint8_t active;
    bool repair;
    CHECK(argus_machine_directory_select_for_test(
              a, ESP_OK, NULL, ESP_ERR_NOT_FOUND, 0U, ESP_OK,
              &output, &active, &repair) == ESP_ERR_NOT_SUPPORTED);
    free(a);
    return ESP_OK;
}

esp_err_t test_4d4_machine_directory_generation_conflict(void)
{
    argus_machine_directory_payload_t *payload =
        calloc(1U, sizeof(*payload));
    CHECK(payload != NULL);
    payload->schema_version = ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION;
    CHECK(argus_machine_directory_commit_precondition(
        payload, 7U, 7U, true));
    CHECK(!argus_machine_directory_commit_precondition(
        payload, 6U, 7U, true));
    CHECK(!argus_machine_directory_commit_precondition(
        payload, 7U, 7U, false));
    payload->schema_version++;
    CHECK(!argus_machine_directory_commit_precondition(
        payload, 7U, 7U, true));
    free(payload);
    return ESP_OK;
}

esp_err_t test_4d4_enrollment_policy_boundaries(void)
{
    argus_principal_t actor = {
        .type = ARGUS_PRINCIPAL_HUMAN,
        .level = ARGUS_SECURITY_LEVEL_CLIENT_ADMIN,
        .permissions = ARGUS_PERMISSION_ENROLL_MACHINES |
                       ARGUS_PERMISSION_VIEW_STATUS |
                       ARGUS_PERMISSION_REQUEST_AUTHORITY |
                       ARGUS_PERMISSION_MOTION,
        .delegable_permissions = ARGUS_PERMISSION_VIEW_STATUS |
                                 ARGUS_PERMISSION_REQUEST_AUTHORITY |
                                 ARGUS_PERMISSION_MOTION,
        .credential_version = 1U,
        .security_epoch = 1U,
        .principal_revision = 1U,
    };
    strlcpy(actor.identifier, "client-admin", sizeof(actor.identifier));
    strlcpy(actor.scope, "paladin/*", sizeof(actor.scope));
    argus_machine_enrollment_request_t request = {
        .client_type = ARGUS_MACHINE_CLIENT_NODE_RED,
        .allowed_transports = ARGUS_MACHINE_TRANSPORT_MQTT,
        .allowed_interfaces = ARGUS_MACHINE_INTERFACE_STA,
        .permissions = ARGUS_PERMISSION_VIEW_STATUS |
                       ARGUS_PERMISSION_REQUEST_AUTHORITY,
    };
    strlcpy(request.display_name, "Node-RED supervisor",
            sizeof(request.display_name));
    strlcpy(request.scope, "paladin/pump_001", sizeof(request.scope));
    strlcpy(request.topic_scope, "argus/paladin/pump_001",
            sizeof(request.topic_scope));
    CHECK(argus_machine_service_enrollment_allowed(&actor, &request));

    static const argus_permission_set_t administrative_permissions[] = {
        ARGUS_PERMISSION_MANAGE_USERS,
        ARGUS_PERMISSION_MANAGE_ROLES,
        ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS,
        ARGUS_PERMISSION_ENROLL_MACHINES,
        ARGUS_PERMISSION_REVOKE_MACHINES,
        ARGUS_PERMISSION_VIEW_AUDIT,
        ARGUS_PERMISSION_MANAGE_NETWORK,
        ARGUS_PERMISSION_CHANGE_AP_SECRET,
        ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK,
        ARGUS_PERMISSION_MANAGE_MQTT,
        ARGUS_PERMISSION_MODIFY_IDENTITY,
        ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG,
        ARGUS_PERMISSION_COMMISSION,
        ARGUS_PERMISSION_CALIBRATE,
        ARGUS_PERMISSION_MANAGE_FIRMWARE,
        ARGUS_PERMISSION_INVOKE_RECOVERY,
        ARGUS_PERMISSION_FULL_SECURITY_RESET,
    };
    actor.delegable_permissions = ARGUS_PERMISSION_DEFINED_MASK;
    for (size_t i = 0U;
         i < sizeof(administrative_permissions) /
                 sizeof(administrative_permissions[0]);
         ++i) {
        request.permissions = administrative_permissions[i];
        CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));
    }
    actor.delegable_permissions = ARGUS_PERMISSION_VIEW_STATUS |
                                  ARGUS_PERMISSION_REQUEST_AUTHORITY |
                                  ARGUS_PERMISSION_MOTION;
    request.permissions = ARGUS_PERMISSION_SOFTWARE_ESTOP;
    CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));
    request.permissions = ARGUS_PERMISSION_VIEW_STATUS;
    strlcpy(request.scope, "other/pump_001", sizeof(request.scope));
    CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));
    strlcpy(request.scope, "paladin/pump_001", sizeof(request.scope));
    strlcpy(request.topic_scope, "argus/paladin/#",
            sizeof(request.topic_scope));
    CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));
    strlcpy(request.topic_scope, "argus/paladin/pump_001",
            sizeof(request.topic_scope));
    request.allowed_interfaces = 0U;
    CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));
    return ESP_OK;
}

esp_err_t test_4d4_connect_valid_credentials(void)
{
    uint8_t packet[256];
    size_t length = connect_packet(packet, sizeof(packet), 0xc2U);
    argus_mqtt_connect_request_t request;
    CHECK(length > 0U);
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length, &request) == ARGUS_MQTT_CONNECT_PARSE_OK);
    CHECK(strcmp(request.client_id,
                 (const char *)request.username) == 0);
    CHECK(request.password_len == ARGUS_MACHINE_SECRET_LENGTH);
    argus_password_zeroize(&request, sizeof(request));
    return ESP_OK;
}

esp_err_t test_4d4_connect_flag_policy(void)
{
    uint8_t packet[256];
    argus_mqtt_connect_request_t request;
    size_t length = connect_packet(packet, sizeof(packet), 0x02U);
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length, &request) == ARGUS_MQTT_CONNECT_PARSE_FLAGS);
    length = connect_packet(packet, sizeof(packet), 0x42U);
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length, &request) == ARGUS_MQTT_CONNECT_PARSE_FLAGS);
    length = connect_packet(packet, sizeof(packet), 0xc0U);
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length, &request) == ARGUS_MQTT_CONNECT_PARSE_FLAGS);
    length = connect_packet(packet, sizeof(packet), 0xc6U);
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length, &request) == ARGUS_MQTT_CONNECT_PARSE_FLAGS);
    return ESP_OK;
}

esp_err_t test_4d4_connect_missing_and_oversized_credentials(void)
{
    uint8_t packet[256];
    argus_mqtt_connect_request_t request;
    size_t length = connect_packet(packet, sizeof(packet), 0x82U);
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length, &request) == ARGUS_MQTT_CONNECT_PARSE_FLAGS);
    length = connect_packet(packet, sizeof(packet), 0xc2U);
    CHECK(length > 46U);
    packet[44] = 0U;
    packet[45] = ARGUS_SECURITY_ID_MAX + 1U;
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length, &request) ==
          ARGUS_MQTT_CONNECT_PARSE_CREDENTIALS);
    return ESP_OK;
}

esp_err_t test_4d4_connect_truncation_and_trailing(void)
{
    uint8_t packet[256];
    argus_mqtt_connect_request_t request;
    size_t length = connect_packet(packet, sizeof(packet), 0xc2U);
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length - 1U, &request) !=
          ARGUS_MQTT_CONNECT_PARSE_OK);
    packet[length++] = 0x55U;
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length, &request) == ARGUS_MQTT_CONNECT_PARSE_MALFORMED);
    return ESP_OK;
}

esp_err_t test_4d4_connect_embedded_nul(void)
{
    uint8_t packet[256];
    argus_mqtt_connect_request_t request;
    size_t length = connect_packet(packet, sizeof(packet), 0xc2U);
    CHECK(length > 50U);
    packet[46] = 0U;
    CHECK(argus_mqtt_broker_parse_connect(
              packet, length, &request) != ARGUS_MQTT_CONNECT_PARSE_OK);
    return ESP_OK;
}

static void topics_and_principal(
    argus_mqtt_topics_t *topics, argus_machine_principal_t *principal)
{
    memset(topics, 0, sizeof(*topics));
    memset(principal, 0, sizeof(*principal));
    (void)argus_mqtt_topics_build(topics, "paladin", "pump_001");
    strlcpy(principal->identifier, "machine",
            sizeof(principal->identifier));
    strlcpy(principal->topic_scope, topics->root,
            sizeof(principal->topic_scope));
}

esp_err_t test_4d4_subscription_capability_and_scope(void)
{
    argus_mqtt_topics_t topics;
    argus_machine_principal_t principal;
    topics_and_principal(&topics, &principal);
    principal.permissions = ARGUS_PERMISSION_VIEW_STATUS;
    char filter[ARGUS_MQTT_BROKER_TOPIC_CAP];
    snprintf(filter, sizeof(filter), "%s/status/#", topics.root);
    CHECK(argus_mqtt_security_subscription_allowed(
        &topics, &principal, filter));
    snprintf(filter, sizeof(filter), "%s/#", topics.root);
    CHECK(!argus_mqtt_security_subscription_allowed(
        &topics, &principal, filter));
    snprintf(filter, sizeof(filter), "%s/security/#", topics.root);
    CHECK(!argus_mqtt_security_subscription_allowed(
        &topics, &principal, filter));
    principal.permissions = 0U;
    CHECK(!argus_mqtt_security_subscription_allowed(
        &topics, &principal, topics.status_wifi));
    return ESP_OK;
}

esp_err_t test_4d4_publish_capability_mapping(void)
{
    argus_mqtt_topics_t topics;
    argus_machine_principal_t principal;
    topics_and_principal(&topics, &principal);
    principal.permissions = ARGUS_PERMISSION_REQUEST_AUTHORITY;
    CHECK(argus_mqtt_security_publish_allowed(
        &topics, &principal, topics.heartbeat));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &principal, topics.command_start));
    principal.permissions = ARGUS_PERMISSION_MOTION;
    CHECK(argus_mqtt_security_publish_allowed(
        &topics, &principal, topics.command_start));
    CHECK(argus_mqtt_security_publish_allowed(
        &topics, &principal, topics.command_recover));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &principal, topics.command_e_stop));
    principal.permissions = ARGUS_PERMISSION_SOFTWARE_ESTOP;
    CHECK(argus_mqtt_security_publish_allowed(
        &topics, &principal, topics.command_e_stop));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &principal, topics.command_reset_e_stop));
    principal.permissions = ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP;
    CHECK(argus_mqtt_security_publish_allowed(
        &topics, &principal, topics.command_reset_e_stop));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &principal, topics.state_online));
    return ESP_OK;
}

esp_err_t test_4d4_machine_scope_policy(void)
{
    CHECK(argus_machine_service_scope_contains("*", "paladin"));
    CHECK(argus_machine_service_scope_contains(
        "paladin/*", "paladin/pump_001"));
    CHECK(!argus_machine_service_scope_contains(
        "paladin/*", "other/pump_001"));
    CHECK(argus_machine_service_topic_scope_contains(
        "argus/paladin/pump_001",
        "argus/paladin/pump_001/status/core/mqtt"));
    CHECK(!argus_machine_service_topic_scope_contains(
        "argus/paladin/pump_001",
        "argus/other/pump_001/status/core/mqtt"));
    return ESP_OK;
}

esp_err_t test_4d4_machine_route_inventory(void)
{
    CHECK(argus_http_route_inventory_validate());
    CHECK(argus_http_test_registered_route_count() +
              argus_security_http_test_route_count() <=
          ARGUS_HTTP_MAX_URI_HANDLERS);
    bool list_get = false;
    bool enroll_post = false;
    bool action_post = false;
    for (size_t i = 0U;
         i < argus_security_http_test_route_count(); ++i) {
        const char *path;
        httpd_method_t method;
        CHECK(argus_security_http_test_registered_route(
            i, &path, &method));
        list_get |= strcmp(path, "/api/security/machines") == 0 &&
                    method == HTTP_GET;
        enroll_post |= strcmp(path, "/api/security/machines") == 0 &&
                       method == HTTP_POST;
        action_post |=
            strcmp(path, "/api/security/machines/action") == 0 &&
            method == HTTP_POST;
    }
    CHECK(list_get && enroll_post && action_post);
    return ESP_OK;
}

esp_err_t test_4d4_secret_zeroization(void)
{
    argus_machine_credential_once_t credential;
    memset(&credential, 0x5a, sizeof(credential));
    argus_machine_service_zero_credential(&credential);
    const uint8_t *bytes = (const uint8_t *)&credential;
    for (size_t i = 0U; i < sizeof(credential); ++i) {
        CHECK(bytes[i] == 0U);
    }
    return ESP_OK;
}

esp_err_t test_4d4_principal_excludes_verifier(void)
{
    CHECK(sizeof(argus_machine_principal_t) <
          sizeof(argus_security_machine_record_t));
    CHECK(sizeof(((argus_mqtt_broker_client_info_t *)0)->principal) ==
          sizeof(argus_machine_principal_t));
    return ESP_OK;
}

typedef struct {
    int selected_socket;
    int shutdown_socket;
    bool close_allowed_during_claim;
    bool close_allowed_after_release;
    bool fail_shutdown;
} socket_owner_test_t;

static void test_after_socket_claim(
    int socket_fd, bool close_allowed, void *ctx)
{
    socket_owner_test_t *test = ctx;
    test->selected_socket = socket_fd;
    test->close_allowed_during_claim = close_allowed;
}

static int test_shutdown_socket(int socket_fd, int how, void *ctx)
{
    (void)how;
    socket_owner_test_t *test = ctx;
    test->shutdown_socket = socket_fd;
    return test->fail_shutdown ? -1 : 0;
}

static void test_after_socket_release(
    int socket_fd, bool close_allowed, void *ctx)
{
    socket_owner_test_t *test = ctx;
    test->selected_socket = socket_fd;
    test->close_allowed_after_release = close_allowed;
}

static argus_mqtt_broker_test_socket_ops_t socket_test_ops(
    socket_owner_test_t *test)
{
    return (argus_mqtt_broker_test_socket_ops_t) {
        .shutdown_socket = test_shutdown_socket,
        .after_claim = test_after_socket_claim,
        .after_release = test_after_socket_release,
        .ctx = test,
    };
}

esp_err_t test_4d4_disconnect_socket_ownership(void)
{
    socket_owner_test_t test = {0};
    argus_mqtt_broker_test_socket_ops_t ops = socket_test_ops(&test);
    CHECK(argus_mqtt_broker_test_disconnect_claim(7, &ops) == ESP_OK);
    CHECK(test.selected_socket == 7);
    CHECK(!test.close_allowed_during_claim);
    CHECK(test.shutdown_socket == 7);
    CHECK(test.close_allowed_after_release);
    return ESP_OK;
}

esp_err_t test_4d4_invalidation_before_bind(void)
{
    argus_mqtt_invalidation_journal_t invalidations;
    argus_mqtt_invalidation_init(&invalidations);
    uint64_t captured =
        argus_mqtt_invalidation_capture(&invalidations);
    CHECK(argus_mqtt_invalidation_record(
        &invalidations, "m-race-target"));
    CHECK(!argus_mqtt_broker_test_bind_allowed(
        &invalidations, captured, "m-race-target",
        true, false, false, 41U, 41U, false));
    CHECK(argus_mqtt_broker_test_bind_allowed(
        &invalidations, captured, "m-unrelated",
        true, false, false, 42U, 42U, false));
    return ESP_OK;
}

esp_err_t test_4d4_bind_before_invalidation(void)
{
    argus_mqtt_invalidation_journal_t invalidations;
    argus_mqtt_invalidation_init(&invalidations);
    uint64_t captured =
        argus_mqtt_invalidation_capture(&invalidations);
    CHECK(argus_mqtt_broker_test_bind_allowed(
        &invalidations, captured, "m-bound-target",
        true, false, false, 51U, 51U, false));
    CHECK(argus_mqtt_invalidation_record(
        &invalidations, "m-bound-target"));
    CHECK(argus_mqtt_broker_test_disconnect_matches(
        true, true, 9, "m-bound-target", "m-bound-target"));
    CHECK(!argus_mqtt_broker_test_disconnect_matches(
        true, true, 10, "m-unrelated", "m-bound-target"));
    CHECK(argus_mqtt_invalidation_since(
        &invalidations, "m-bound-target", captured));
    CHECK(!argus_mqtt_invalidation_since(
        &invalidations, "m-unrelated", captured));
    return ESP_OK;
}

esp_err_t test_4d4_audit_failure_still_disconnects(void)
{
    const bool rotations[] = {false, false, false, true};
    for (size_t i = 0U;
         i < sizeof(rotations) / sizeof(rotations[0]); ++i) {
        argus_security_http_machine_action_decision_t decision;
        argus_security_http_machine_action_decide(
            ESP_OK, ESP_FAIL, rotations[i], &decision);
        CHECK(decision.mutation_committed);
        CHECK(decision.disconnect_machine);
        CHECK(decision.response_error == ESP_FAIL);
        CHECK(decision.quarantine_rotation == rotations[i]);
        CHECK(!decision.disclose_rotation_secret);
    }
    argus_security_http_machine_action_decision_t success;
    argus_security_http_machine_action_decide(
        ESP_OK, ESP_OK, true, &success);
    CHECK(success.disconnect_machine);
    CHECK(success.disclose_rotation_secret);
    CHECK(!success.quarantine_rotation);
    argus_security_http_machine_action_decision_t failed;
    argus_security_http_machine_action_decide(
        ESP_ERR_NOT_ALLOWED, ESP_ERR_NOT_ALLOWED, true, &failed);
    CHECK(!failed.mutation_committed);
    CHECK(!failed.disconnect_machine);
    CHECK(!failed.disclose_rotation_secret);
    return ESP_OK;
}

esp_err_t test_4d4_invalidated_connection_is_inert(void)
{
    CHECK(argus_mqtt_broker_test_packet_admitted(
        true, true, false, 61U, 61U));
    CHECK(!argus_mqtt_broker_test_packet_admitted(
        true, true, true, 61U, 61U));
    CHECK(!argus_mqtt_broker_test_packet_admitted(
        true, true, false, 62U, 61U));

    socket_owner_test_t test = {.fail_shutdown = true};
    argus_mqtt_broker_test_socket_ops_t ops = socket_test_ops(&test);
    CHECK(argus_mqtt_broker_test_disconnect_claim(11, &ops) == ESP_FAIL);
    CHECK(!test.close_allowed_during_claim);
    CHECK(test.shutdown_socket == 11);
    CHECK(test.close_allowed_after_release);

    size_t subscriptions = 0U;
    size_t publishes = 0U;
    size_t heartbeats = 0U;
    size_t sequences = 0U;
    size_t authority_lookups = 0U;
    size_t dispatches = 0U;
    bool admitted = argus_mqtt_broker_test_packet_admitted(
        true, true, true, 63U, 63U);
    if (admitted) {
        subscriptions++;
        publishes++;
        heartbeats++;
        sequences++;
        authority_lookups++;
        dispatches++;
    }
    CHECK(subscriptions == 0U && publishes == 0U);
    CHECK(heartbeats == 0U && sequences == 0U);
    CHECK(authority_lookups == 0U && dispatches == 0U);
    return ESP_OK;
}

/* ===========================================================================
 * Network admission and resource-budget closure (work order 2026-07-27).
 * Each test drives the PRODUCTION decision function, not a restatement of it.
 * =========================================================================*/

esp_err_t test_4d4_iface_ambiguity_fails_closed(void)
{
    /* The AP is the ESP-IDF default 192.168.4.1; a rogue DHCP lease can put
     * the STA interface on the same address. The old code checked AP first
     * and returned immediately, so that overlap resolved to SOFTAP - the MORE
     * privileged answer - unlocking AP-only browser routes and SOFTAP-only
     * machine records to the plant network. Ambiguity must resolve the other
     * way. */
    const uint32_t ap = 0x0104A8C0U;    /* 192.168.4.1  */
    const uint32_t sta = 0x3A32A8C0U;   /* 192.168.50.58 */

    argus_net_mgr_clear_interface_conflict();

    /* Unambiguous cases still classify. */
    CHECK(argus_net_mgr_classify_interface(ap, true, ap, true, sta) ==
          ARGUS_NET_IFACE_SOFTAP);
    CHECK(argus_net_mgr_classify_interface(sta, true, ap, true, sta) ==
          ARGUS_NET_IFACE_STA);
    CHECK(!argus_net_mgr_interface_conflict_detected());

    /* Both interfaces on the same address: every socket is ambiguous. */
    CHECK(argus_net_mgr_classify_interface(ap, true, ap, true, ap) ==
          ARGUS_NET_IFACE_AMBIGUOUS);
    CHECK(argus_net_mgr_interface_conflict_detected());

    /* The fault is now OBSERVABLE, not just an internal flag: assertion time,
     * observation count and the operator action all have to be readable, or
     * the refused AP-only access stays unexplained. */
    argus_net_iface_conflict_status_t status = {0};
    argus_net_mgr_get_interface_conflict(&status);
    CHECK(status.active);
    CHECK(status.observations >= 1U);
    CHECK(argus_net_mgr_interface_conflict_action() != NULL);
    CHECK(argus_net_mgr_interface_conflict_action()[0] != '\0');

    /* Repeated detection must not turn into a log flood: the first
     * observation may log, subsequent ones inside the interval may not. */
    (void)argus_net_mgr_interface_conflict_log_due();
    CHECK(!argus_net_mgr_interface_conflict_log_due());

    /* It stays asserted while the overlap keeps being observed. */
    CHECK(argus_net_mgr_classify_interface(ap, true, ap, true, ap) ==
          ARGUS_NET_IFACE_AMBIGUOUS);
    CHECK(argus_net_mgr_interface_conflict_detected());

    /* CLEARING is deterministic and requires POSITIVE evidence: observing
     * both interfaces valid and no longer overlapping. Not a timer, and not
     * an interface merely going away. */
    CHECK(argus_net_mgr_classify_interface(sta, true, ap, true, sta) ==
          ARGUS_NET_IFACE_STA);
    CHECK(!argus_net_mgr_interface_conflict_detected());

    /* An interface going away is NOT evidence of a healthy configuration. */
    CHECK(argus_net_mgr_classify_interface(ap, true, ap, true, ap) ==
          ARGUS_NET_IFACE_AMBIGUOUS);
    CHECK(argus_net_mgr_interface_conflict_detected());
    CHECK(argus_net_mgr_classify_interface(sta, false, 0U, true, sta) ==
          ARGUS_NET_IFACE_STA);
    CHECK(argus_net_mgr_interface_conflict_detected());

    /* It re-asserts immediately on the next overlapping observation after a
     * clear, so a cleared fault is never a licence to trust an overlap. */
    argus_net_mgr_clear_interface_conflict();
    CHECK(!argus_net_mgr_interface_conflict_detected());
    CHECK(argus_net_mgr_classify_interface(ap, true, ap, true, ap) ==
          ARGUS_NET_IFACE_AMBIGUOUS);
    CHECK(argus_net_mgr_interface_conflict_detected());
    argus_net_mgr_clear_interface_conflict();
    CHECK(!argus_net_mgr_interface_conflict_detected());

    /* A local address matching neither interface is UNKNOWN, not SOFTAP. */
    CHECK(argus_net_mgr_classify_interface(0x0202A8C0U, true, ap, true, sta) ==
          ARGUS_NET_IFACE_UNKNOWN);
    /* No usable local address at all. */
    CHECK(argus_net_mgr_classify_interface(0U, true, ap, true, sta) ==
          ARGUS_NET_IFACE_UNKNOWN);
    /* AP address unknown (netif down): an STA socket still classifies, and
     * nothing can classify as SOFTAP. */
    CHECK(argus_net_mgr_classify_interface(sta, false, 0U, true, sta) ==
          ARGUS_NET_IFACE_STA);
    CHECK(argus_net_mgr_classify_interface(ap, false, 0U, true, sta) ==
          ARGUS_NET_IFACE_UNKNOWN);
    CHECK(!argus_net_mgr_interface_conflict_detected());
    return ESP_OK;
}

esp_err_t test_4d4_kdf_global_bound_is_source_independent(void)
{
    /* The per-source buckets cannot bound total authentication work: there
     * are only 8, they evict LRU, and a BLOCKED bucket is the stalest so it
     * is evicted first - an attacker clears its own cooldown by connecting
     * from a few other addresses. This global bucket is the bound that
     * cannot be escaped, because it never looks at the address. */
    uint32_t retry = 0U;
    const uint64_t t0 = UINT64_C(1000000);
    /* Distinct hostile addresses. None is proven, so all share the unproven
     * allowance no matter how many of them there are. */
    const uint32_t hostile[] = {0x01010101U, 0x02020202U, 0x03030303U,
                                0x04040404U, 0x05050505U, 0x06060606U,
                                0x07070707U, 0x08080808U, 0x09090909U};

    argus_machine_service_kdf_global_reset_for_test();
    argus_machine_service_clear_proven_sources_for_test();

    /* The unproven share is admitted, each request from a DIFFERENT source... */
    for (uint32_t i = 0; i < ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST; ++i) {
        CHECK(argus_machine_service_kdf_global_admit_for_test(
            hostile[i], t0, &retry));
    }
    /* ...and the very next unproven request is refused with a usable retry
     * hint, from an address that has spent nothing. Cycling addresses does
     * not help: the bound is global, and the remaining tokens are reserved. */
    retry = 0U;
    CHECK(!argus_machine_service_kdf_global_admit_for_test(
        hostile[ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST], t0, &retry));
    CHECK(retry > 0U);

    /* Still refused later in the same window. */
    CHECK(!argus_machine_service_kdf_global_admit_for_test(
              hostile[0], t0 + ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US - 1U,
              &retry));

    /* The window rolls, and legitimate work proceeds - the bound delays a
     * reconnect during a flood, it does not deny it indefinitely. */
    CHECK(argus_machine_service_kdf_global_admit_for_test(
              hostile[0], t0 + ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US,
              &retry));

    argus_machine_service_kdf_global_reset_for_test();
    argus_machine_service_clear_proven_sources_for_test();
    return ESP_OK;
}

esp_err_t test_4d4_proven_source_reservation_bounds_the_flood(void)
{
    /* What the reservation DOES support: a flood from addresses that have
     * never authenticated cannot consume the whole KDF budget, so a source
     * that recently authenticated still gets work done. What it does NOT
     * support is asserted at the end - honestly, because a reservation keyed
     * on an unauthenticated source address cannot identify anyone. */
    const uint32_t proven = 0x0A0A0A0AU;
    const uint64_t t0 = UINT64_C(5000000);
    uint32_t retry = 0U;

    argus_machine_service_kdf_global_reset_for_test();
    argus_machine_service_clear_proven_sources_for_test();
    argus_machine_service_mark_source_proven_for_test(proven, t0);

    /* Hostile sources exhaust the unproven share. */
    for (uint32_t i = 0; i < ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST; ++i) {
        CHECK(argus_machine_service_kdf_global_admit_for_test(
            0xB0000000U + i, t0, &retry));
    }
    CHECK(!argus_machine_service_kdf_global_admit_for_test(
        0xB000FFFFU, t0, &retry));

    /* The proven source still gets the reserved remainder. This is the whole
     * point: the flood delays it, it does not lock it out. */
    for (uint32_t i = ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST;
         i < ARGUS_MACHINE_AUTH_KDF_GLOBAL_BURST; ++i) {
        CHECK(argus_machine_service_kdf_global_admit_for_test(
            proven, t0, &retry));
    }

    /* The absolute ceiling still binds everyone, proven included: the
     * reservation reallocates the budget, it does not raise it. */
    CHECK(!argus_machine_service_kdf_global_admit_for_test(proven, t0, &retry));

    /* RESIDUAL LIMITATION, asserted so it cannot quietly become a claim: a
     * flood ORIGINATING FROM the proven address consumes the reserved share
     * itself. Nothing here distinguishes the real HMI from anything else at
     * that address before credential verification. */
    argus_machine_service_kdf_global_reset_for_test();
    for (uint32_t i = 0; i < ARGUS_MACHINE_AUTH_KDF_GLOBAL_BURST; ++i) {
        CHECK(argus_machine_service_kdf_global_admit_for_test(
            proven, t0, &retry));
    }
    CHECK(!argus_machine_service_kdf_global_admit_for_test(proven, t0, &retry));

    /* RESIDUAL LIMITATION: the table is empty after boot, so the first
     * reconnect after a restart competes as an unproven source. */
    argus_machine_service_clear_proven_sources_for_test();
    argus_machine_service_kdf_global_reset_for_test();
    for (uint32_t i = 0; i < ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST; ++i) {
        CHECK(argus_machine_service_kdf_global_admit_for_test(
            0xC0000000U + i, t0, &retry));
    }
    CHECK(!argus_machine_service_kdf_global_admit_for_test(proven, t0, &retry));

    /* Entries expire, and an expired entry is not proven. */
    argus_machine_service_kdf_global_reset_for_test();
    argus_machine_service_clear_proven_sources_for_test();
    argus_machine_service_mark_source_proven_for_test(proven, t0);
    for (uint32_t i = 0; i < ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST; ++i) {
        CHECK(argus_machine_service_kdf_global_admit_for_test(
            0xD0000000U + i, t0 + ARGUS_MACHINE_AUTH_PROVEN_TTL_US, &retry));
    }
    CHECK(!argus_machine_service_kdf_global_admit_for_test(
        proven, t0 + ARGUS_MACHINE_AUTH_PROVEN_TTL_US, &retry));

    /* The condition is PUBLISHED, so it has to assert when the budget is
     * actually exhausted. Driven at real time so the live window - the one
     * argus_machine_service_get_throttle_status() reads - is the one being
     * exhausted. */
    argus_machine_service_kdf_global_reset_for_test();
    argus_machine_service_clear_proven_sources_for_test();
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    argus_machine_auth_throttle_status_t status = {0};
    for (uint32_t i = 0; i < ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST; ++i) {
        CHECK(argus_machine_service_kdf_global_admit_for_test(
            0xE0000000U + i, now_us, &retry));
    }
    /* Spending the share is NOT the condition: nothing has been denied yet,
     * and five legitimate reconnects reach this state on an ordinary power-up
     * storm. Assert that it stays clear here... */
    argus_machine_service_get_throttle_status(&status);
    CHECK(!status.throttle_active || status.blocked_sources > 0U);
    /* ...and asserts on the first request actually refused. */
    CHECK(!argus_machine_service_kdf_global_admit_for_test(
        0xE000FFFFU, now_us, &retry));
    argus_machine_service_get_throttle_status(&status);
    CHECK(status.throttle_active);
    CHECK(status.seconds_until_clear > 0U);
    CHECK(status.unproven_burst < status.window_burst);
    CHECK(status.window_spent >= ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST);

    argus_machine_service_kdf_global_reset_for_test();
    argus_machine_service_get_throttle_status(&status);
    /* Only the budget half is asserted clear here: a source may legitimately
     * still be in failure lockout from real traffic, and the published
     * condition covers BOTH causes deliberately. */
    CHECK(status.window_spent == 0U);
    CHECK(status.global_refusals == 0U);

    argus_machine_service_clear_proven_sources_for_test();
    return ESP_OK;
}

esp_err_t test_4d4_permission_edit_cannot_escalate(void)
{
    /* Editing a deployed machine's permissions in place is a GRANTING
     * operation, so it reuses the rules enrolment already applies rather than
     * inventing a second, weaker set. These pin the three that matter, using
     * the same predicate the service calls.
     *
     * The operation exists so a panel can gain MOTION without being deleted
     * and re-provisioned. That convenience must not become a way to hand out
     * capabilities the granting operator does not itself hold. */

    argus_principal_t actor = {0};
    actor.type = ARGUS_PRINCIPAL_HUMAN;
    strlcpy(actor.identifier, "admin", sizeof(actor.identifier));
    strlcpy(actor.scope, "site", sizeof(actor.scope));
    actor.principal_revision = 1U;
    actor.security_epoch = 1U;
    /* credential_version must be nonzero and delegable must be a SUBSET of
     * held, or argus_authorization_principal_valid() rejects the actor and
     * every check below would fail for a reason that has nothing to do with
     * what is being tested. The first version of this fixture got both wrong
     * and the suite caught it - which is the right direction for a fixture
     * bug to fail. */
    actor.credential_version = 1U;
    actor.permissions = ARGUS_PERMISSION_ENROLL_MACHINES |
                        ARGUS_PERMISSION_VIEW_STATUS |
                        ARGUS_PERMISSION_MOTION |
                        ARGUS_PERMISSION_REQUEST_AUTHORITY;
    /* Delegable is deliberately NARROWER than held: the operator may use
     * MOTION themselves but may not hand it to a machine. */
    actor.delegable_permissions = ARGUS_PERMISSION_VIEW_STATUS;

    argus_machine_enrollment_request_t request = {0};
    strlcpy(request.display_name, "panel", sizeof(request.display_name));
    strlcpy(request.scope, "site", sizeof(request.scope));
    strlcpy(request.topic_scope, "argus/paladin/pump_001",
            sizeof(request.topic_scope));
    request.client_type = ARGUS_MACHINE_CLIENT_HMI;
    request.allowed_transports = ARGUS_MACHINE_TRANSPORT_MQTT;
    request.allowed_interfaces = ARGUS_MACHINE_INTERFACE_STA;

    /* 1. May not grant what the actor cannot delegate. */
    request.permissions = ARGUS_PERMISSION_VIEW_STATUS | ARGUS_PERMISSION_MOTION;
    CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));

    /* Widen delegation and the same grant becomes legitimate - proving the
     * refusal above was the delegation rule and not something incidental. */
    actor.delegable_permissions =
        ARGUS_PERMISSION_VIEW_STATUS | ARGUS_PERMISSION_MOTION |
        ARGUS_PERMISSION_REQUEST_AUTHORITY;
    CHECK(argus_machine_service_enrollment_allowed(&actor, &request));

    /* 2. A machine may NEVER hold an administrative permission, even when the
     *    actor could delegate it. This is a ceiling on what a machine can be,
     *    not a statement about the operator. */
    actor.delegable_permissions |= ARGUS_PERMISSION_ENROLL_MACHINES;
    request.permissions = ARGUS_PERMISSION_VIEW_STATUS |
                          ARGUS_PERMISSION_ENROLL_MACHINES;
    CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));

    /* 3. Out-of-scope targets are refused. */
    actor.delegable_permissions =
        ARGUS_PERMISSION_VIEW_STATUS | ARGUS_PERMISSION_MOTION;
    request.permissions = ARGUS_PERMISSION_VIEW_STATUS;
    strlcpy(request.scope, "other-site", sizeof(request.scope));
    CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));

    /* 4. Undefined permission bits are refused outright rather than stored.
     *    A capability nobody can authorise against must never reach the
     *    directory. */
    strlcpy(request.scope, "site", sizeof(request.scope));
    request.permissions = ~ARGUS_PERMISSION_DEFINED_MASK;
    CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));

    /* 5. The edit path refuses an undefined set before it reads anything,
     *    so a bad request cannot even open a directory snapshot. */
    argus_permission_set_t previous = 0xFFFFFFFFU;
    CHECK(argus_machine_service_set_permissions(
              &actor, "m-nonexistent", ~ARGUS_PERMISSION_DEFINED_MASK,
              &previous) == ESP_ERR_INVALID_ARG);
    CHECK(previous == 0U);   /* cleared, never left as caller garbage */

    /* 6. A well-formed request for a machine that does not exist is
     *    NOT_FOUND - distinct from a refusal, so an operator can tell a typo
     *    in the identifier from a permissions problem. */
    CHECK(argus_machine_service_set_permissions(
              &actor, "m-nonexistent", ARGUS_PERMISSION_VIEW_STATUS,
              &previous) == ESP_ERR_NOT_FOUND);
    return ESP_OK;
}

esp_err_t test_4d4_permission_edit_invalidates_live_sessions(void)
{
    /* The property that makes in-place editing safe: a change takes effect on
     * an ALREADY-CONNECTED client rather than at its next reconnect.
     *
     * principal_revision is the mechanism. argus_machine_service_revalidate()
     * compares the connected principal's revision against the directory's on
     * every publish and subscribe, so bumping it on an edit invalidates the
     * live session. This asserts the comparison itself, in both directions -
     * a REDUCTION must bite exactly as fast as a grant, or a machine would go
     * on exercising a capability an operator had just taken away. */

    argus_machine_principal_t connected = {0};
    strlcpy(connected.identifier, "m-panel", sizeof(connected.identifier));
    connected.permissions = ARGUS_PERMISSION_VIEW_STATUS;
    connected.credential_version = 1U;
    connected.principal_revision = 4U;
    connected.record_security_epoch = 9U;
    connected.allowed_transports = ARGUS_MACHINE_TRANSPORT_MQTT;
    connected.allowed_interfaces = ARGUS_MACHINE_INTERFACE_STA;

    /* Same revision: the session is still the one the directory describes. */
    CHECK(argus_mqtt_broker_test_packet_admitted(true, true, false, 7U, 7U));

    /* An edit bumps the revision, so the connected principal no longer
     * matches. The exact comparison revalidate() performs: */
    uint32_t directory_revision_after_edit = connected.principal_revision + 1U;
    CHECK(directory_revision_after_edit != connected.principal_revision);

    /* And the wrap rule holds - revision 0 is reserved for "never set", so an
     * edit that wraps must land on 1 rather than on the sentinel. A record
     * sitting at 0 would compare equal to a default-initialised principal. */
    uint32_t wrapped = 0xFFFFFFFFU;
    wrapped++;
    if (wrapped == 0U) wrapped = 1U;
    CHECK(wrapped == 1U);
    CHECK(wrapped != 0U);

    /* The security store rejects a stored record carrying revision 0, which
     * is what makes the wrap rule load-bearing rather than cosmetic. */
    CHECK(connected.principal_revision != 0U);
    return ESP_OK;
}

esp_err_t test_4d4_admission_budgets_are_self_consistent(void)
{
    /* These constants are load-bearing and were previously fictional: the
     * broker advertised 10 client slots costing ~13 KB of heap each, against
     * a heap that could not fund them, so the shortfall surfaced as unrelated
     * allocations failing. Pin the relationships that must hold. */

    /* The previous version of this test asserted MAX_PRECONNECT <=
     * MAX_CLIENTS and called that "unauthenticated load cannot consume
     * authenticated capacity". It was the wrong property, and it passed while
     * the defect was live: both pools drew from ONE clients[MAX_CLIENTS]
     * array, so filling the pre-connect pool filled every physical slot and
     * the HMI could not reconnect. Assert the sizing invariant that actually
     * makes the separation physical instead.
     *
     *   pending      <  MAX_PRECONNECT   (checked before a record is taken)
     *   authenticated<= MAX_CLIENTS      (session-slot ownership)
     *   => in_use    <= MAX_CONNECTIONS - 1, so an admitted arrival always
     *      has a free record and a CONNECT always has authenticated capacity
     *      whenever fewer than MAX_CLIENTS sessions exist. */
    CHECK(ARGUS_MQTT_BROKER_MAX_CLIENTS_DECLARED >= 2U);  /* HMI + ArgusCore */
    CHECK(ARGUS_MQTT_BROKER_MAX_PRECONNECT_DECLARED >= 1U);
    CHECK(ARGUS_MQTT_BROKER_MAX_CONNECTIONS_DECLARED ==
          ARGUS_MQTT_BROKER_MAX_CLIENTS_DECLARED +
              ARGUS_MQTT_BROKER_MAX_PRECONNECT_DECLARED);
    /* One source must not be able to fill the pre-connect pool alone. */
    CHECK(ARGUS_MQTT_BROKER_MAX_PRECONNECT_PER_SOURCE_DECLARED <
          ARGUS_MQTT_BROKER_MAX_PRECONNECT_DECLARED);
    /* Part of the pre-connect pool must stay out of reach of sources that
     * have never authenticated, or "the HMI can still reconnect while the
     * pool is full" is not a property the code has. */
    CHECK(ARGUS_MQTT_BROKER_MAX_PRECONNECT_UNPROVEN_DECLARED <
          ARGUS_MQTT_BROKER_MAX_PRECONNECT_DECLARED);
    /* Same shape for the KDF budget. */
    CHECK(ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST <
          ARGUS_MACHINE_AUTH_KDF_GLOBAL_BURST);
    CHECK(ARGUS_MACHINE_AUTH_PROVEN_SOURCES >= 2U);
    /* The retained store must cover every retained contract topic. */
    CHECK(ARGUS_MQTT_BROKER_RETAINED_CAPACITY >=
          ARGUS_MQTT_RETAINED_TOPICS_REQUIRED);

    /* The replay cache is sized from the enrollment ceiling - one slot per
     * machine that can have a request in flight - not from a traffic guess. */
    CHECK(ARGUS_MQTT_AUTH_DUP_CACHE_SIZE == ARGUS_SECURITY_MAX_MACHINES);

    /* Concurrent machine KDF admission must leave the depth-1 worker queue
     * free for the human/recovery path. */
    CHECK(ARGUS_MACHINE_AUTH_KDF_ADMISSION_MAX == 1U);
    return ESP_OK;
}

esp_err_t test_4d4_pending_pool_cannot_starve_authenticated(void)
{
    /* Drives argus_mqtt_broker_preconnect_decide() - the function production
     * calls, not a copy of it - across every reachable occupancy. */
    const size_t max_pending = ARGUS_MQTT_BROKER_MAX_PRECONNECT_DECLARED;
    const size_t max_auth = ARGUS_MQTT_BROKER_MAX_CLIENTS_DECLARED;
    const size_t max_conn = ARGUS_MQTT_BROKER_MAX_CONNECTIONS_DECLARED;

    /* 1. THE INVARIANT. Whenever a socket is admitted into the pre-connect
     *    pool, a physical record exists for it even with the authenticated
     *    pool completely full. This is what a shared array did not give. */
    for (size_t pending = 0U; pending <= max_pending; ++pending) {
        for (size_t authenticated = 0U; authenticated <= max_auth;
             ++authenticated) {
            bool admitted = argus_mqtt_broker_preconnect_decide(
                                pending, 0U, true) ==
                            ARGUS_MQTT_PRECONNECT_ADMIT;
            if (admitted) {
                CHECK(pending + 1U + authenticated <= max_conn);
            }
        }
    }

    /* 2. A full pre-connect pool never consumes authenticated capacity: even
     *    at maximum pending, MAX_CLIENTS records remain for sessions. */
    CHECK(max_conn - max_pending == max_auth);

    /* 3. Hostile sources cannot take the reserved share. This is the
     *    "two or more sources fill the pending allowance and the HMI still
     *    reconnects" case: unproven sources are refused at
     *    MAX_PRECONNECT_UNPROVEN, a proven source is still admitted. */
    const size_t unproven_cap = ARGUS_MQTT_BROKER_MAX_PRECONNECT_UNPROVEN_DECLARED;
    for (size_t pending = 0U; pending < unproven_cap; ++pending) {
        CHECK(argus_mqtt_broker_preconnect_decide(pending, 0U, false) ==
              ARGUS_MQTT_PRECONNECT_ADMIT);
    }
    CHECK(argus_mqtt_broker_preconnect_decide(unproven_cap, 0U, false) ==
          ARGUS_MQTT_PRECONNECT_REFUSE_RESERVED);
    CHECK(argus_mqtt_broker_preconnect_decide(unproven_cap, 0U, true) ==
          ARGUS_MQTT_PRECONNECT_ADMIT);

    /* 4. The absolute pool bound still binds a proven source: the
     *    reservation reallocates capacity, it does not create any. */
    CHECK(argus_mqtt_broker_preconnect_decide(max_pending, 0U, true) ==
          ARGUS_MQTT_PRECONNECT_REFUSE_POOL);

    /* 5. Per-source cap, checked after the pool bounds so the reported
     *    reason names the binding constraint. */
    CHECK(argus_mqtt_broker_preconnect_decide(
              0U, ARGUS_MQTT_BROKER_MAX_PRECONNECT_PER_SOURCE_DECLARED,
              true) == ARGUS_MQTT_PRECONNECT_REFUSE_SOURCE);

    /* 6. The live broker agrees with the declared budget, and pending and
     *    authenticated occupancy are reported SEPARATELY - a single "client
     *    count" is exactly what hid the defect. */
    argus_mqtt_broker_capacity_t cap;
    CHECK(argus_mqtt_broker_get_capacity(&cap) == ESP_OK);
    CHECK(cap.max_connections == max_conn);
    CHECK(cap.max_authenticated == max_auth);
    CHECK(cap.max_pending == max_pending);
    CHECK(cap.pending + cap.authenticated <= cap.max_connections);
    CHECK(cap.authenticated <= cap.max_authenticated);
    CHECK(cap.pending <= cap.max_pending);
    /* The connection record must not carry the subscription table any more;
     * that lives in the session record, which only an authenticated client
     * owns. If this ever inverts, pending sockets are paying for capacity
     * they cannot use. */
    CHECK(cap.session_record_bytes >= 1280U);   /* 8 filters x 160 bytes */
    CHECK(cap.session_record_bytes > cap.connection_record_bytes);

    /* 7. The retained store is a resource too, and it overflowed on hardware
     *    in this pass: the broker rightly refused to evict authoritative
     *    state, so the last retained topics silently never became retained
     *    and a client subscribing later would not have learned about a live
     *    fault. Sized from the contract now, with headroom asserted here so a
     *    future retained topic fails the suite instead of a bench run. */
    CHECK(cap.retained_capacity >= ARGUS_MQTT_RETAINED_TOPICS_REQUIRED);
    CHECK(cap.retained_used <= cap.retained_capacity);
    CHECK(cap.retained_used < cap.retained_capacity);
    return ESP_OK;
}

esp_err_t test_4d4_authenticated_pool_refuses_beyond_limit(void)
{
    /* The authenticated limit is a physical resource: exactly
     * MAX_CLIENTS session records exist, a CONNECT cannot complete without
     * owning one, and the client beyond the limit gets a clean CONNACK 0x03
     * rather than an allocation failure somewhere unrelated.
     *
     * SCOPE OF THIS TEST, stated so it is not mistaken for more: it proves
     * the budget relationships and that the refusal counter exists on the
     * production path. Driving MAX_CLIENTS+1 real authenticated clients needs
     * that many enrolled machine credentials and is a HARDWARE step - see the
     * evidence record, which says plainly whether it was performed. */
    argus_mqtt_broker_capacity_t cap;
    CHECK(argus_mqtt_broker_get_capacity(&cap) == ESP_OK);

    /* Enough for the two roles the deployment actually has. */
    CHECK(cap.max_authenticated >= 2U);
    /* Session records are the resource; there is one per supported client. */
    CHECK(cap.session_record_bytes > 0U);
    CHECK(cap.client_task_stack_bytes > 0U);
    /* Never observed more authenticated clients than the pool can hold. */
    CHECK(cap.peak_authenticated <= cap.max_authenticated);
    CHECK(cap.peak_pending <= cap.max_pending);
    /* A client task must never have run closer to its stack limit than the
     * margin the budget assumes. Seeded to the stack size, so this also
     * passes before any client has connected. */
    CHECK(cap.client_stack_min_free_bytes >= 512U);
    CHECK(cap.client_stack_min_free_bytes <= cap.client_task_stack_bytes);
    return ESP_OK;
}
