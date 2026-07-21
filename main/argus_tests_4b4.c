/**
 * @file argus_tests_4b4.c
 * @brief Exhaustive pure tests for the Phase 4B.4 request decoder.
 */

#include "argus_tests_4b4.h"

#include "argus_browser_command.h"

#include <string.h>

#define TEST_CHECK(condition) do { if (!(condition)) return ESP_FAIL; } while (0)

typedef struct {
    const char *json;
    argus_browser_command_decode_result_t expected;
} decode_failure_case_t;

static bool request_is_invalid(const argus_browser_command_request_t *request)
{
    return request != NULL && !request->is_valid &&
           (int)request->command_type == -1 &&
           request->target_rpm_milli == 0 && !request->forward;
}

static esp_err_t expect_failure_bytes(
    const uint8_t *body,
    size_t body_len,
    argus_browser_command_decode_result_t expected)
{
    argus_browser_command_request_t request = {
        .is_valid = true,
        .command_type = ARGUS_CMD_TYPE_START,
        .target_rpm_milli = 12345,
        .forward = true,
    };

    argus_browser_command_decode_result_t result =
        argus_browser_command_decode(body, body_len, &request);
    TEST_CHECK(result == expected);
    TEST_CHECK(request_is_invalid(&request));
    return ESP_OK;
}

static esp_err_t expect_failure(const decode_failure_case_t *test_case)
{
    return expect_failure_bytes((const uint8_t *)test_case->json,
                                strlen(test_case->json),
                                test_case->expected);
}

esp_err_t test_4b4_decode_argument_free_commands(void)
{
    static const struct {
        const char *json;
        argus_cmd_type_t expected_type;
    } cases[] = {
        {"{\"command\":\"start\"}", ARGUS_CMD_TYPE_START},
        {"{\"command\":\"stop\"}", ARGUS_CMD_TYPE_STOP_NORMAL},
        {"{\"command\":\"unlock\"}", ARGUS_CMD_TYPE_UNLOCK},
        {"{\"command\":\"estop\"}", ARGUS_CMD_TYPE_ESTOP},
        {"{\"command\":\"reset_estop\"}", ARGUS_CMD_TYPE_RESET_ESTOP},
        {"{\"command\":\"recover\"}", ARGUS_CMD_TYPE_RECOVER},
        {" \r\n{\"\\u0063ommand\":\"st\\u0061rt\"}\t ", ARGUS_CMD_TYPE_START},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        argus_browser_command_request_t request;
        TEST_CHECK(argus_browser_command_decode(
                       (const uint8_t *)cases[i].json,
                       strlen(cases[i].json), &request) ==
                   ARGUS_BROWSER_CMD_DECODE_OK);
        TEST_CHECK(request.is_valid);
        TEST_CHECK(request.command_type == cases[i].expected_type);
        TEST_CHECK(request.target_rpm_milli == 0);
        TEST_CHECK(!request.forward);
    }
    return ESP_OK;
}

esp_err_t test_4b4_decode_set_target_success(void)
{
    static const struct {
        const char *json;
        int32_t target;
        bool forward;
    } cases[] = {
        {"{\"command\":\"set_target\",\"target_rpm_milli\":0,\"forward\":true}", 0, true},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":false}", 500, false},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":200000,\"forward\":true}", 200000, true},
        {"{\"target_rpm_milli\":500,\"forward\":true,\"command\":\"set_target\"}", 500, true},
        {"{\"forward\":false,\"command\":\"set_target\",\"target_rpm_milli\":500}", 500, false},
        {"{\"target_rpm_milli\":500,\"command\":\"set_target\",\"forward\":true}", 500, true},
        {"{\"forward\":true,\"target_rpm_milli\":500,\"command\":\"set_target\"}", 500, true},
        {"{\"command\":\"set_target\",\"forward\":false,\"target_rpm_milli\":500}", 500, false},
        {"{\"forward\":true,\"command\":\"set_target\",\"target_rpm_milli\":500}", 500, true},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        argus_browser_command_request_t request;
        TEST_CHECK(argus_browser_command_decode(
                       (const uint8_t *)cases[i].json,
                       strlen(cases[i].json), &request) ==
                   ARGUS_BROWSER_CMD_DECODE_OK);
        TEST_CHECK(request.is_valid);
        TEST_CHECK(request.command_type == ARGUS_CMD_TYPE_SET_TARGET);
        TEST_CHECK(request.target_rpm_milli == cases[i].target);
        TEST_CHECK(request.forward == cases[i].forward);
    }
    return ESP_OK;
}

esp_err_t test_4b4_decode_malformed_and_top_level(void)
{
    static const decode_failure_case_t cases[] = {
        {"", ARGUS_BROWSER_CMD_DECODE_EMPTY_BODY},
        {" \t\r\n ", ARGUS_BROWSER_CMD_DECODE_EMPTY_BODY},
        {"[\"start\"]", ARGUS_BROWSER_CMD_DECODE_TOP_LEVEL_NOT_OBJECT},
        {"null", ARGUS_BROWSER_CMD_DECODE_TOP_LEVEL_NOT_OBJECT},
        {"true", ARGUS_BROWSER_CMD_DECODE_TOP_LEVEL_NOT_OBJECT},
        {"42", ARGUS_BROWSER_CMD_DECODE_TOP_LEVEL_NOT_OBJECT},
        {"\"start\"", ARGUS_BROWSER_CMD_DECODE_TOP_LEVEL_NOT_OBJECT},
        {"{", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\"", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\":", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\":\"start\"", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\":\"start}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{command:\"start\"}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\" \"start\"}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\":\"start\",}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\":\"st\\qart\"}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\":\"st\\u12\"}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\":\"st\\uD800art\"}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\":\"st\\u0000art\"}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_CHECK(expect_failure(&cases[i]) == ESP_OK);
    }
    return ESP_OK;
}

esp_err_t test_4b4_decode_command_rejections(void)
{
    static const decode_failure_case_t cases[] = {
        {"{}", ARGUS_BROWSER_CMD_DECODE_MISSING_FIELD},
        {"{\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_MISSING_FIELD},
        {"{\"command\":\"pause\"}", ARGUS_BROWSER_CMD_DECODE_UNSUPPORTED_COMMAND},
        {"{\"command\":\"Start\"}", ARGUS_BROWSER_CMD_DECODE_UNSUPPORTED_COMMAND},
        {"{\"command\":null}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":true}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":1}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":[]}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":{}}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"start\",\"command\":\"stop\"}", ARGUS_BROWSER_CMD_DECODE_DUPLICATE_FIELD},
        {"{\"command\":\"start\",\"\\u0063ommand\":\"stop\"}", ARGUS_BROWSER_CMD_DECODE_DUPLICATE_FIELD},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_CHECK(expect_failure(&cases[i]) == ESP_OK);
    }
    return ESP_OK;
}

esp_err_t test_4b4_decode_field_contract_rejections(void)
{
    static const decode_failure_case_t cases[] = {
        {"{\"command\":\"set_target\",\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_MISSING_FIELD},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500}", ARGUS_BROWSER_CMD_DECODE_MISSING_FIELD},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"target_rpm_milli\":600,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_DUPLICATE_FIELD},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":true,\"forward\":false}", ARGUS_BROWSER_CMD_DECODE_DUPLICATE_FIELD},
        {"{\"command\":\"start\",\"extra\":1}", ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD},
        {"{\"command\":\"start\",\"extra\":1,\"extra\":2}", ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":true,\"extra\":1}", ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD},
        {"{\"command\":\"start\",\"target_rpm_milli\":500}", ARGUS_BROWSER_CMD_DECODE_UNEXPECTED_FIELD},
        {"{\"command\":\"stop\",\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_UNEXPECTED_FIELD},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_CHECK(expect_failure(&cases[i]) == ESP_OK);
    }
    return ESP_OK;
}

esp_err_t test_4b4_decode_target_rejections(void)
{
    static const decode_failure_case_t cases[] = {
        {"{\"command\":\"set_target\",\"target_rpm_milli\":-1,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_VALUE_OUT_OF_RANGE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":-0,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_VALUE_OUT_OF_RANGE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":200001,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_VALUE_OUT_OF_RANGE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":999999999999999999999999,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_VALUE_OUT_OF_RANGE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":1.0,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":1e3,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":\"500\",\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":true,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":null,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":[],\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":{},\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":01,\"forward\":true}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_CHECK(expect_failure(&cases[i]) == ESP_OK);
    }
    return ESP_OK;
}

esp_err_t test_4b4_decode_forward_rejections(void)
{
    static const decode_failure_case_t cases[] = {
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":0}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":1}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":\"true\"}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":null}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":[]}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":{}}", ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE},
        {"{\"command\":\"set_target\",\"target_rpm_milli\":500,\"forward\":truex}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_CHECK(expect_failure(&cases[i]) == ESP_OK);
    }
    return ESP_OK;
}

esp_err_t test_4b4_decode_length_trailing_and_nul(void)
{
    static const decode_failure_case_t trailing_cases[] = {
        {"{\"command\":\"start\"}{}", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
        {"{\"command\":\"start\"}garbage", ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON},
    };
    for (size_t i = 0; i < sizeof(trailing_cases) / sizeof(trailing_cases[0]); i++) {
        TEST_CHECK(expect_failure(&trailing_cases[i]) == ESP_OK);
    }

    static const uint8_t embedded_nul[] = {
        '{', '"', 'c', 'o', 'm', 'm', 'a', 'n', 'd', '"', ':',
        '"', 's', 't', 'a', 'r', 't', '"', '}', 0, 'x'
    };
    TEST_CHECK(expect_failure_bytes(embedded_nul, sizeof(embedded_nul),
                                    ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON) == ESP_OK);
    static const uint8_t string_nul[] = {
        '{', '"', 'c', 'o', 'm', 'm', 'a', 'n', 'd', '"', ':',
        '"', 's', 't', 'a', 0, 'r', 't', '"', '}'
    };
    TEST_CHECK(expect_failure_bytes(string_nul, sizeof(string_nul),
                                    ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON) == ESP_OK);

    static const char valid[] = "{\"command\":\"start\"}";
    uint8_t max_body[ARGUS_BROWSER_COMMAND_MAX_BODY_LEN];
    memset(max_body, ' ', sizeof(max_body));
    memcpy(max_body, valid, sizeof(valid) - 1U);
    argus_browser_command_request_t request;
    TEST_CHECK(argus_browser_command_decode(max_body, sizeof(max_body), &request) ==
               ARGUS_BROWSER_CMD_DECODE_OK);
    TEST_CHECK(request.is_valid && request.command_type == ARGUS_CMD_TYPE_START);

    uint8_t oversized_body[ARGUS_BROWSER_COMMAND_MAX_BODY_LEN + 1U];
    memset(oversized_body, ' ', sizeof(oversized_body));
    memcpy(oversized_body, valid, sizeof(valid) - 1U);
    TEST_CHECK(expect_failure_bytes(oversized_body, sizeof(oversized_body),
                                    ARGUS_BROWSER_CMD_DECODE_BODY_TOO_LARGE) == ESP_OK);
    return ESP_OK;
}

esp_err_t test_4b4_decode_output_reuse_and_routing_rejections(void)
{
    argus_browser_command_request_t request;
    static const char valid[] = "{\"command\":\"start\"}";
    TEST_CHECK(argus_browser_command_decode((const uint8_t *)valid,
                                            sizeof(valid) - 1U,
                                            &request) == ARGUS_BROWSER_CMD_DECODE_OK);
    TEST_CHECK(request.is_valid);

    static const char invalid[] = "{\"command\":\"Start\"}";
    TEST_CHECK(argus_browser_command_decode((const uint8_t *)invalid,
                                            sizeof(invalid) - 1U,
                                            &request) ==
               ARGUS_BROWSER_CMD_DECODE_UNSUPPORTED_COMMAND);
    TEST_CHECK(request_is_invalid(&request));

    static const decode_failure_case_t routing_cases[] = {
        {"{\"command\":\"start\",\"source\":\"portal\"}", ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD},
        {"{\"command\":\"start\",\"\\u0073ource\":\"portal\"}", ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD},
        {"{\"command\":\"start\",\"authority_generation\":1}", ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD},
        {"{\"command\":\"start\",\"pump_id\":1}", ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD},
        {"{\"command\":\"start\",\"channel_id\":1}", ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD},
    };
    for (size_t i = 0; i < sizeof(routing_cases) / sizeof(routing_cases[0]); i++) {
        TEST_CHECK(expect_failure(&routing_cases[i]) == ESP_OK);
    }

    TEST_CHECK(argus_browser_command_decode(NULL, 1U, &request) ==
               ARGUS_BROWSER_CMD_DECODE_INVALID_ARGUMENT);
    TEST_CHECK(request_is_invalid(&request));
    TEST_CHECK(argus_browser_command_decode((const uint8_t *)valid,
                                            sizeof(valid) - 1U, NULL) ==
               ARGUS_BROWSER_CMD_DECODE_INVALID_ARGUMENT);
    return ESP_OK;
}
