#include "argus_tests_4d4.h"

#include <stdlib.h>
#include <string.h>

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

    /* The conflict latch is sticky - anything admitted while it held stays
     * suspect until an operator clears it. */
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

    argus_machine_service_kdf_global_reset_for_test();

    /* The burst is admitted... */
    for (uint32_t i = 0; i < ARGUS_MACHINE_AUTH_KDF_GLOBAL_BURST; ++i) {
        CHECK(argus_machine_service_kdf_global_admit_for_test(t0, &retry));
    }
    /* ...and the very next one is refused, with a usable retry hint. No
     * source address was ever supplied, so no amount of address cycling
     * changes this. */
    retry = 0U;
    CHECK(!argus_machine_service_kdf_global_admit_for_test(t0, &retry));
    CHECK(retry > 0U);

    /* Still refused later in the same window. */
    CHECK(!argus_machine_service_kdf_global_admit_for_test(
              t0 + ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US - 1U, &retry));

    /* The window rolls, and legitimate work proceeds - the bound delays a
     * reconnect during a flood, it does not deny it indefinitely. */
    CHECK(argus_machine_service_kdf_global_admit_for_test(
              t0 + ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US, &retry));

    argus_machine_service_kdf_global_reset_for_test();
    return ESP_OK;
}

esp_err_t test_4d4_admission_budgets_are_self_consistent(void)
{
    /* These constants are load-bearing and were previously fictional: the
     * broker advertised 10 client slots costing ~13 KB of heap each, against
     * a heap that could not fund them, so the shortfall surfaced as unrelated
     * allocations failing. Pin the relationships that must hold. */

    /* Unauthenticated load must never be able to consume the authenticated
     * capacity: the pre-connect pool cannot exceed the client pool. */
    CHECK(ARGUS_MQTT_BROKER_MAX_CLIENTS_DECLARED >= 1U);
    CHECK(ARGUS_MQTT_BROKER_MAX_PRECONNECT_DECLARED <=
          ARGUS_MQTT_BROKER_MAX_CLIENTS_DECLARED);
    /* One source must not be able to fill the pre-connect pool alone. */
    CHECK(ARGUS_MQTT_BROKER_MAX_PRECONNECT_PER_SOURCE_DECLARED <
          ARGUS_MQTT_BROKER_MAX_PRECONNECT_DECLARED);

    /* The replay cache is sized from the enrollment ceiling - one slot per
     * machine that can have a request in flight - not from a traffic guess. */
    CHECK(ARGUS_MQTT_AUTH_DUP_CACHE_SIZE == ARGUS_SECURITY_MAX_MACHINES);

    /* Concurrent machine KDF admission must leave the depth-1 worker queue
     * free for the human/recovery path. */
    CHECK(ARGUS_MACHINE_AUTH_KDF_ADMISSION_MAX == 1U);
    return ESP_OK;
}
