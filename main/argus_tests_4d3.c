#include "argus_tests_4d3.h"

#include <stdio.h>
#include <string.h>

#include "argus_auth_service.h"
#include "argus_authorization.h"
#include "argus_cmd_parser.h"
#include "argus_http_server.h"
#include "argus_http_security.h"
#include "argus_password_verifier.h"
#include "argus_security_directory.h"
#include "argus_security_admin.h"
#include "argus_security_http.h"
#include "argus_session_manager.h"

#define CHECK(condition) do { if (!(condition)) return ESP_FAIL; } while (0)

typedef struct {
    const uint8_t *values;
    size_t value_count;
    size_t calls;
} random_trace_t;

static esp_err_t traced_random(void *ctx, uint8_t *out, size_t length)
{
    random_trace_t *trace = (random_trace_t *)ctx;
    if (trace == NULL || out == NULL ||
        trace->calls >= trace->value_count) {
        return ESP_FAIL;
    }
    memset(out, trace->values[trace->calls++], length);
    return ESP_OK;
}

static argus_principal_t principal(
    const char *identifier,
    const char *scope,
    argus_security_level_t level)
{
    argus_principal_t result = {
        .type = ARGUS_PRINCIPAL_HUMAN,
        .level = level,
        .permissions = argus_authorization_level_permissions(level),
        .delegable_permissions =
            argus_authorization_level_delegable(level),
        .credential_version = 1U,
        .security_epoch = 1U,
        .principal_revision = 1U,
    };
    strlcpy(result.identifier, identifier, sizeof(result.identifier));
    strlcpy(result.scope, scope, sizeof(result.scope));
    return result;
}

static void valid_verifier(argus_password_verifier_t *record)
{
    memset(record, 0, sizeof(*record));
    record->format_version = ARGUS_PASSWORD_FORMAT_VERSION;
    record->algorithm = ARGUS_PASSWORD_ALGORITHM_PBKDF2_HMAC_SHA256;
    record->salt_length = ARGUS_PASSWORD_SALT_SIZE;
    record->verifier_length = ARGUS_PASSWORD_VERIFIER_SIZE;
    record->iterations = ARGUS_PASSWORD_ITERATIONS_MIN;
    memset(record->salt, 0x31, sizeof(record->salt));
    memset(record->verifier, 0x72, sizeof(record->verifier));
}

static bool contains(
    const char *data, size_t data_length, const char *needle)
{
    size_t length = needle == NULL ? 0U : strlen(needle);
    if (data == NULL || length == 0U || length > data_length) return false;
    for (size_t i = 0U; i <= data_length - length; ++i) {
        if (memcmp(data + i, needle, length) == 0) return true;
    }
    return false;
}

esp_err_t test_4d3_username_policy(void)
{
    char canonical[ARGUS_LOGIN_NAME_MAX + 1U];
    CHECK(argus_auth_canonicalize_login("Shawn.Admin", canonical));
    CHECK(strcmp(canonical, "shawn.admin") == 0);
    CHECK(argus_auth_canonicalize_login("user_name-1", canonical));
    CHECK(!argus_auth_canonicalize_login("", canonical));
    CHECK(!argus_auth_canonicalize_login(" leading", canonical));
    CHECK(!argus_auth_canonicalize_login("trailing ", canonical));
    CHECK(!argus_auth_canonicalize_login("bad\tname", canonical));
    char oversized[ARGUS_LOGIN_NAME_MAX + 2U];
    memset(oversized, 'a', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    CHECK(!argus_auth_canonicalize_login(oversized, canonical));
    return ESP_OK;
}

esp_err_t test_4d3_password_policy(void)
{
    static const uint8_t minimum[] = "twelve chars!";
    static const uint8_t spaces[] = "correct horse battery staple";
    CHECK(argus_auth_new_password_valid(
        minimum, sizeof(minimum) - 1U));
    CHECK(argus_auth_new_password_valid(
        spaces, sizeof(spaces) - 1U));
    CHECK(!argus_auth_new_password_valid(
        (const uint8_t *)"too short", 9U));
    uint8_t maximum[ARGUS_PASSWORD_INPUT_MAX];
    memset(maximum, 'x', sizeof(maximum));
    CHECK(argus_auth_new_password_valid(maximum, sizeof(maximum)));
    uint8_t control[12] = "valid value";
    control[4] = '\n';
    CHECK(!argus_auth_new_password_valid(control, sizeof(control)));
    CHECK(!argus_auth_new_password_valid(
        maximum, sizeof(maximum) + 1U));
    return ESP_OK;
}

esp_err_t test_4d3_authorization_role_matrix(void)
{
    argus_permission_set_t argus =
        argus_authorization_level_permissions(
            ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL);
    argus_permission_set_t admin =
        argus_authorization_level_permissions(
            ARGUS_SECURITY_LEVEL_CLIENT_ADMIN);
    argus_permission_set_t viewer =
        argus_authorization_level_permissions(
            ARGUS_SECURITY_LEVEL_VIEWER);
    CHECK(argus == ARGUS_PERMISSION_DEFINED_MASK);
    CHECK((admin & ARGUS_PERMISSION_MANAGE_USERS) != 0U);
    CHECK((admin & ARGUS_PERMISSION_MANAGE_NETWORK) == 0U);
    CHECK(viewer == ARGUS_PERMISSION_VIEW_STATUS);
    CHECK((argus_authorization_level_delegable(
               ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL) &
           ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) == 0U);
    CHECK(ARGUS_PERMISSION_DEFINED_MASK ==
          ((UINT64_C(1) << 23U) - 1U));
    for (argus_security_level_t level =
             ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL;
         level <= ARGUS_SECURITY_LEVEL_VIEWER; ++level) {
        argus_principal_t subject = principal("matrix", "paladin", level);
        argus_permission_set_t expected =
            argus_authorization_level_permissions(level);
        for (uint32_t bit = 0U; bit < 23U; ++bit) {
            argus_permission_set_t capability = UINT64_C(1) << bit;
            argus_authorization_result_t result =
                argus_authorization_require(&subject, capability);
            CHECK(result ==
                ((expected & capability) != 0U
                    ? ARGUS_AUTHZ_ALLOW
                    : ARGUS_AUTHZ_DENY_CAPABILITY));
        }
    }
    argus_principal_t unknown = principal(
        "unknown", "paladin", ARGUS_SECURITY_LEVEL_VIEWER);
    unknown.level = (argus_security_level_t)UINT8_MAX;
    CHECK(!argus_authorization_principal_valid(&unknown));
    return ESP_OK;
}

esp_err_t test_4d3_authorization_capability_denials(void)
{
    argus_principal_t viewer = principal(
        "viewer", "paladin", ARGUS_SECURITY_LEVEL_VIEWER);
    CHECK(argus_authorization_principal_valid(&viewer));
    CHECK(argus_authorization_require(
              &viewer, ARGUS_PERMISSION_VIEW_STATUS) ==
          ARGUS_AUTHZ_ALLOW);
    CHECK(argus_authorization_require(
              &viewer, ARGUS_PERMISSION_MOTION) ==
          ARGUS_AUTHZ_DENY_CAPABILITY);
    CHECK(argus_authorization_require(&viewer, 0U) ==
          ARGUS_AUTHZ_DENY_INVALID);
    viewer.permissions |= UINT64_C(1) << 40U;
    CHECK(!argus_authorization_principal_valid(&viewer));
    return ESP_OK;
}

esp_err_t test_4d3_authorization_target_ceilings(void)
{
    argus_principal_t admin = principal(
        "admin", "paladin", ARGUS_SECURITY_LEVEL_CLIENT_ADMIN);
    CHECK(argus_authorization_manage_target(
              &admin, ARGUS_SECURITY_LEVEL_OPERATOR,
              "paladin", false) == ARGUS_AUTHZ_ALLOW);
    CHECK(argus_authorization_manage_target(
              &admin, ARGUS_SECURITY_LEVEL_OPERATOR,
              "other", false) == ARGUS_AUTHZ_DENY_SCOPE);
    CHECK(argus_authorization_manage_target(
              &admin, ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL,
              "paladin", false) == ARGUS_AUTHZ_DENY_PROTECTED);
    CHECK(argus_authorization_manage_target(
              &admin, ARGUS_SECURITY_LEVEL_OPERATOR,
              "paladin", true) == ARGUS_AUTHZ_DENY_PROTECTED);
    CHECK(argus_authorization_manage_target(
              &admin, ARGUS_SECURITY_LEVEL_CLIENT_ADMIN,
              "paladin", false) == ARGUS_AUTHZ_DENY_CAPABILITY);
    admin.permissions |= ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS;
    CHECK(argus_authorization_manage_target(
              &admin, ARGUS_SECURITY_LEVEL_CLIENT_ADMIN,
              "paladin", false) == ARGUS_AUTHZ_ALLOW);

    argus_security_directory_payload_t directory = {
        .schema_version = ARGUS_SECURITY_DIRECTORY_SCHEMA_VERSION,
        .human_count = 2U,
    };
    directory.humans[0].level = ARGUS_SECURITY_LEVEL_CLIENT_ADMIN;
    directory.humans[0].enabled = 1U;
    directory.humans[0].role_mask =
        UINT16_C(1) << ARGUS_SECURITY_LEVEL_CLIENT_ADMIN;
    directory.humans[1].level = ARGUS_SECURITY_LEVEL_VIEWER;
    directory.humans[1].enabled = 1U;
    directory.humans[1].role_mask =
        UINT16_C(1) << ARGUS_SECURITY_LEVEL_VIEWER;
    CHECK(argus_security_admin_removal_would_lock_out(
        &directory, 0U));
    directory.humans[1].level = ARGUS_SECURITY_LEVEL_CLIENT_ADMIN;
    directory.humans[1].role_mask =
        UINT16_C(1) << ARGUS_SECURITY_LEVEL_CLIENT_ADMIN;
    CHECK(!argus_security_admin_removal_would_lock_out(
        &directory, 0U));
    CHECK(!argus_security_admin_removal_would_lock_out(
        &directory, 1U));
    return ESP_OK;
}

esp_err_t test_4d3_authorization_delegation_ceiling(void)
{
    argus_principal_t admin = principal(
        "admin", "paladin", ARGUS_SECURITY_LEVEL_CLIENT_ADMIN);
    CHECK(argus_authorization_delegate(
              &admin, ARGUS_SECURITY_LEVEL_OPERATOR, "paladin",
              ARGUS_PERMISSION_VIEW_STATUS |
              ARGUS_PERMISSION_MOTION) == ARGUS_AUTHZ_ALLOW);
    CHECK(argus_authorization_delegate(
              &admin, ARGUS_SECURITY_LEVEL_OPERATOR, "paladin",
              ARGUS_PERMISSION_MANAGE_NETWORK) ==
          ARGUS_AUTHZ_DENY_DELEGATION);
    admin.permissions |= ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS;
    CHECK(argus_authorization_delegate(
              &admin, ARGUS_SECURITY_LEVEL_CLIENT_ADMIN, "paladin",
              ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS) ==
          ARGUS_AUTHZ_DENY_DELEGATION);
    return ESP_OK;
}

esp_err_t test_4d3_session_issue_authenticate_and_csrf(void)
{
    argus_session_table_t table;
    argus_session_table_init(&table);
    argus_principal_t user = principal(
        "operator", "paladin", ARGUS_SECURITY_LEVEL_OPERATOR);
    const uint8_t values[] = {0x11, 0x22};
    random_trace_t random = {values, 2U, 0U};
    argus_session_credentials_t credentials;
    CHECK(argus_session_create(
              &table, &user, 1U, traced_random, &random,
              &credentials) == ESP_OK);
    CHECK(strlen(credentials.token) == ARGUS_SESSION_TOKEN_HEX_LEN);
    CHECK(strlen(credentials.csrf) == ARGUS_SESSION_TOKEN_HEX_LEN);
    argus_principal_t authenticated;
    size_t index = ARGUS_SESSION_CAPACITY;
    CHECK(argus_session_authenticate(
              &table, credentials.token, 2U,
              &authenticated, &index) == ESP_OK);
    CHECK(index < ARGUS_SESSION_CAPACITY);
    CHECK(strcmp(authenticated.identifier, "operator") == 0);
    CHECK(argus_session_csrf_valid(&table, index, credentials.csrf));
    char csrf_copy[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
    CHECK(argus_session_get_csrf(&table, index, csrf_copy));
    CHECK(strcmp(csrf_copy, credentials.csrf) == 0);
    CHECK(!contains(
        (const char *)&table, sizeof(table), credentials.token));
    credentials.csrf[0] = credentials.csrf[0] == 'a' ? 'b' : 'a';
    CHECK(!argus_session_csrf_valid(&table, index, credentials.csrf));
    return ESP_OK;
}

esp_err_t test_4d3_session_population_limits(void)
{
    argus_session_table_t table;
    argus_session_table_init(&table);
    argus_principal_t user = principal(
        "same", "paladin", ARGUS_SECURITY_LEVEL_OPERATOR);
    uint8_t values[6] = {1, 2, 3, 4, 5, 6};
    random_trace_t random = {values, 6U, 0U};
    argus_session_credentials_t credentials;
    CHECK(argus_session_create(
              &table, &user, 1U, traced_random, &random,
              &credentials) == ESP_OK);
    CHECK(argus_session_create(
              &table, &user, 2U, traced_random, &random,
              &credentials) == ESP_OK);
    CHECK(argus_session_create(
              &table, &user, 3U, traced_random, &random,
              &credentials) == ESP_ERR_NO_MEM);

    argus_session_table_init(&table);
    uint8_t capacity_values[ARGUS_SESSION_CAPACITY * 2U];
    for (size_t i = 0U; i < sizeof(capacity_values); ++i) {
        capacity_values[i] = (uint8_t)(i + 1U);
    }
    random = (random_trace_t) {
        capacity_values, sizeof(capacity_values), 0U,
    };
    for (size_t i = 0U; i < ARGUS_SESSION_CAPACITY; ++i) {
        char id[12];
        snprintf(id, sizeof(id), "user%u", (unsigned)i);
        user = principal(id, "paladin", ARGUS_SECURITY_LEVEL_VIEWER);
        CHECK(argus_session_create(
                  &table, &user, i + 1U, traced_random, &random,
                  &credentials) == ESP_OK);
    }
    user = principal("overflow", "paladin", ARGUS_SECURITY_LEVEL_VIEWER);
    CHECK(argus_session_create(
              &table, &user, 20U, traced_random, &random,
              &credentials) == ESP_ERR_NO_MEM);
    return ESP_OK;
}

esp_err_t test_4d3_session_expiry_boundaries(void)
{
    argus_session_table_t table;
    argus_session_table_init(&table);
    argus_principal_t user = principal(
        "operator", "paladin", ARGUS_SECURITY_LEVEL_OPERATOR);
    const uint8_t values[] = {0x31, 0x32};
    random_trace_t random = {values, 2U, 0U};
    argus_session_credentials_t credentials;
    CHECK(argus_session_create(
              &table, &user, 1U, traced_random, &random,
              &credentials) == ESP_OK);
    argus_principal_t out;
    CHECK(argus_session_authenticate(
              &table, credentials.token,
              ARGUS_SESSION_IDLE_US, &out, NULL) == ESP_OK);
    CHECK(argus_session_authenticate(
              &table, credentials.token,
              ARGUS_SESSION_IDLE_US * 2U, &out, NULL) ==
          ESP_ERR_NOT_FOUND);

    argus_session_table_init(&table);
    random.calls = 0U;
    CHECK(argus_session_create(
              &table, &user, 1U, traced_random, &random,
              &credentials) == ESP_OK);
    table.records[0].last_activity_us =
        ARGUS_SESSION_ABSOLUTE_US - 1U;
    CHECK(argus_session_authenticate(
              &table, credentials.token,
              ARGUS_SESSION_ABSOLUTE_US, &out, NULL) == ESP_OK);
    CHECK(argus_session_authenticate(
              &table, credentials.token,
              ARGUS_SESSION_ABSOLUTE_US + 1U, &out, NULL) ==
          ESP_ERR_NOT_FOUND);

    argus_session_table_init(&table);
    random.calls = 0U;
    CHECK(argus_session_create(
              &table, &user, 1U, traced_random, &random,
              &credentials) == ESP_OK);
    CHECK(argus_session_recently_reauthenticated(
        &table, 0U, ARGUS_SESSION_REAUTH_US));
    CHECK(!argus_session_recently_reauthenticated(
        &table, 0U, ARGUS_SESSION_REAUTH_US + 1U));
    argus_session_mark_reauthenticated(
        &table, 0U, ARGUS_SESSION_REAUTH_US + 2U);
    CHECK(argus_session_recently_reauthenticated(
        &table, 0U, ARGUS_SESSION_REAUTH_US * 2U));
    return ESP_OK;
}

esp_err_t test_4d3_session_collision_retry(void)
{
    argus_session_table_t table;
    argus_session_table_init(&table);
    argus_session_credentials_t first;
    argus_session_credentials_t second;
    argus_principal_t one = principal(
        "one", "paladin", ARGUS_SECURITY_LEVEL_VIEWER);
    argus_principal_t two = principal(
        "two", "paladin", ARGUS_SECURITY_LEVEL_VIEWER);
    const uint8_t first_values[] = {0x41, 0x42};
    random_trace_t random = {first_values, 2U, 0U};
    CHECK(argus_session_create(
              &table, &one, 1U, traced_random, &random, &first) == ESP_OK);
    const uint8_t collision_values[] = {0x41, 0x43, 0x44};
    random = (random_trace_t) {collision_values, 3U, 0U};
    CHECK(argus_session_create(
              &table, &two, 2U, traced_random, &random, &second) == ESP_OK);
    CHECK(random.calls == 3U);
    CHECK(strcmp(first.token, second.token) != 0);
    return ESP_OK;
}

esp_err_t test_4d3_session_revocation(void)
{
    argus_session_table_t table;
    argus_session_table_init(&table);
    argus_principal_t user = principal(
        "operator", "paladin", ARGUS_SECURITY_LEVEL_OPERATOR);
    const uint8_t values[] = {0x51, 0x52, 0x53, 0x54};
    random_trace_t random = {values, 4U, 0U};
    argus_session_credentials_t one;
    argus_session_credentials_t two;
    CHECK(argus_session_create(
              &table, &user, 1U, traced_random, &random, &one) == ESP_OK);
    CHECK(argus_session_create(
              &table, &user, 2U, traced_random, &random, &two) == ESP_OK);
    CHECK(argus_session_revoke_token(&table, one.token));
    CHECK(argus_session_count(&table) == 1U);
    CHECK(argus_session_revoke_principal(&table, "operator") == 1U);
    CHECK(argus_session_count(&table) == 0U);

    random.calls = 0U;
    user.security_epoch = 7U;
    CHECK(argus_session_create(
              &table, &user, 3U, traced_random, &random, &one) == ESP_OK);
    CHECK(argus_session_revoke_epoch(&table, 8U) == 1U);
    CHECK(argus_session_count(&table) == 0U);
    return ESP_OK;
}

esp_err_t test_4d3_session_malformed_tokens(void)
{
    argus_session_table_t table;
    argus_session_table_init(&table);
    argus_principal_t out;
    CHECK(argus_session_authenticate(
              &table, "", 1U, &out, NULL) == ESP_ERR_NOT_FOUND);
    CHECK(argus_session_authenticate(
              &table, "not-hex", 1U, &out, NULL) == ESP_ERR_NOT_FOUND);
    char oversized[ARGUS_SESSION_TOKEN_HEX_LEN + 2U];
    memset(oversized, 'a', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    CHECK(argus_session_authenticate(
              &table, oversized, 1U, &out, NULL) ==
          ESP_ERR_NOT_FOUND);
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    char token[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
    char valid_cookie[160];
    memset(token, 'a', ARGUS_SESSION_TOKEN_HEX_LEN);
    token[ARGUS_SESSION_TOKEN_HEX_LEN] = '\0';
    snprintf(valid_cookie, sizeof(valid_cookie),
             "theme=dark; " ARGUS_SESSION_COOKIE_NAME "=%s", token);
    char parsed[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
    CHECK(argus_http_security_test_parse_cookie(valid_cookie, parsed));
    CHECK(strcmp(parsed, token) == 0);
    char duplicate[240];
    snprintf(duplicate, sizeof(duplicate),
             ARGUS_SESSION_COOKIE_NAME "=%s; "
             ARGUS_SESSION_COOKIE_NAME "=%s", token, token);
    CHECK(!argus_http_security_test_parse_cookie(duplicate, parsed));
    CHECK(!argus_http_security_test_parse_cookie(
        ARGUS_SESSION_COOKIE_NAME "=short", parsed));
    CHECK(!argus_http_security_test_parse_cookie(
        "broken-cookie", parsed));
#endif
    return ESP_OK;
}

esp_err_t test_4d3_directory_role_integrity(void)
{
    argus_security_directory_payload_t payload = {
        .schema_version = ARGUS_SECURITY_DIRECTORY_SCHEMA_VERSION,
        .human_count = 1U,
    };
    argus_security_human_record_t *human = &payload.humans[0];
    human->record_version = ARGUS_SECURITY_RECORD_VERSION;
    human->level = ARGUS_SECURITY_LEVEL_VIEWER;
    human->enabled = 1U;
    human->role_mask =
        (uint16_t)(UINT16_C(1) << ARGUS_SECURITY_LEVEL_VIEWER);
    human->credential_version = 1U;
    human->record_security_epoch = 1U;
    strlcpy(human->identifier, "viewer-1", sizeof(human->identifier));
    strlcpy(human->login, "viewer", sizeof(human->login));
    strlcpy(human->display_name, "Viewer", sizeof(human->display_name));
    strlcpy(human->scope, "paladin", sizeof(human->scope));
    valid_verifier(&human->verifier);
    CHECK(argus_security_directory_payload_valid(&payload));
    human->role_mask |= UINT16_C(1) << ARGUS_SECURITY_BUILTIN_ROLE_COUNT;
    CHECK(!argus_security_directory_payload_valid(&payload));
    human->role_mask =
        UINT16_C(1) << ARGUS_SECURITY_LEVEL_OPERATOR;
    CHECK(!argus_security_directory_payload_valid(&payload));
    return ESP_OK;
}

esp_err_t test_4d3_login_decoder_strictness(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    static const char valid[] =
        "{\"username\":\"argus\",\"password\":\"local password\"}";
    static const char duplicate[] =
        "{\"username\":\"argus\",\"username\":\"other\","
        "\"password\":\"local password\"}";
    static const char unknown[] =
        "{\"username\":\"argus\",\"password\":\"local password\","
        "\"extra\":true}";
    static const char trailing[] =
        "{\"username\":\"argus\",\"password\":\"local password\"} trailing";
    static const char top_level[] = "[]";
    CHECK(argus_http_test_decode_login(
        (const uint8_t *)valid, sizeof(valid) - 1U));
    CHECK(!argus_http_test_decode_login(
        (const uint8_t *)duplicate, sizeof(duplicate) - 1U));
    CHECK(!argus_http_test_decode_login(
        (const uint8_t *)unknown, sizeof(unknown) - 1U));
    CHECK(!argus_http_test_decode_login(
        (const uint8_t *)trailing, sizeof(trailing) - 1U));
    CHECK(!argus_http_test_decode_login(
        (const uint8_t *)top_level, sizeof(top_level) - 1U));
    CHECK(!argus_http_test_decode_login(
        (const uint8_t *)"{\"username\":", 12U));
#endif
    return ESP_OK;
}

esp_err_t test_4d3_command_capability_mapping(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    CHECK(argus_http_test_command_capability(ARGUS_CMD_TYPE_ESTOP) ==
          ARGUS_PERMISSION_SOFTWARE_ESTOP);
    CHECK(argus_http_test_command_capability(ARGUS_CMD_TYPE_RESET_ESTOP) ==
          ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP);
    CHECK(argus_http_test_command_capability(ARGUS_CMD_TYPE_START) ==
          ARGUS_PERMISSION_MOTION);
    CHECK(argus_http_test_command_capability(ARGUS_CMD_TYPE_STOP_NORMAL) ==
          ARGUS_PERMISSION_MOTION);
    CHECK(argus_http_test_command_capability(ARGUS_CMD_TYPE_SET_TARGET) ==
          ARGUS_PERMISSION_MOTION);
    CHECK(argus_http_test_command_capability(UINT32_MAX) == 0U);
#endif
    return ESP_OK;
}

esp_err_t test_4d3_browser_artifact_contract(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    size_t length = 0U;
    const char *page = argus_http_test_commission_page(&length);
    CHECK(page != NULL && length > 10000U);
    CHECK(contains(page, length, "/api/auth/session"));
    CHECK(contains(page, length, "X-Argus-CSRF"));
    CHECK(contains(page, length, "/api/security/accounts"));
    CHECK(contains(page, length, "/api/security/roles"));
    CHECK(contains(page, length, "/api/security/audit"));
    CHECK(contains(page, length, "/api/security/ap-password"));
    CHECK(contains(page, length, "/api/security/recovery/exit"));
    CHECK(contains(page, length, "/api/auth/change-password"));
    CHECK(!contains(page, length, "localStorage"));
    CHECK(!contains(page, length, "sessionStorage"));
    CHECK(!contains(page, length, "Authorization: Basic"));
    CHECK(!contains(page, length, "WWW-Authenticate"));
    page = argus_http_test_controls_page(&length);
    CHECK(page != NULL && length > 10000U);
    CHECK(contains(page, length, "/api/auth/session"));
    CHECK(contains(page, length, "runtime.caps.has(\"motion\")"));
    CHECK(contains(page, length, "runtime.caps.has(\"software_estop\")"));
    CHECK(contains(
        page, length,
        "runtime.caps.has(\"reset_software_estop\")"));
#endif
    return ESP_OK;
}

esp_err_t test_4d3_security_route_inventory(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    CHECK(argus_security_http_test_route_count() == 8U);
    CHECK(argus_http_test_command_registration());
    CHECK(argus_http_test_factory_reset_registration());
    static const uint8_t ipv4[] = {192U, 168U, 4U, 1U};
    static const uint8_t mapped[] = {
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0xffU, 0xffU, 192U, 168U, 4U, 1U,
    };
    static const uint8_t native_ipv6[] = {
        0xfeU, 0x80U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U,
    };
    uint32_t direct = 0U;
    uint32_t normalized = 0U;
    CHECK(argus_http_security_test_normalize_address(
        false, ipv4, sizeof(ipv4), &direct));
    CHECK(argus_http_security_test_normalize_address(
        true, mapped, sizeof(mapped), &normalized));
    CHECK(direct == normalized);
    CHECK(!argus_http_security_test_normalize_address(
        true, native_ipv6, sizeof(native_ipv6), &normalized));
#endif
    return ESP_OK;
}
