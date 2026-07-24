#include "argus_tests_4d4.h"

#include <stdlib.h>
#include <string.h>

#include "argus_http_route_inventory.h"
#include "argus_machine_directory.h"
#include "argus_machine_service.h"
#include "argus_mqtt_broker.h"
#include "argus_mqtt_contract.h"
#include "argus_mqtt_security.h"
#include "argus_password_verifier.h"
#include "argus_security_http.h"

#define CHECK(value) do { if (!(value)) return ESP_FAIL; } while (0)

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
    static const uint8_t id[] = "m-0123456789abcdef0123456789abcdef";
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

    request.permissions |= ARGUS_PERMISSION_MANAGE_USERS;
    CHECK(!argus_machine_service_enrollment_allowed(&actor, &request));
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
