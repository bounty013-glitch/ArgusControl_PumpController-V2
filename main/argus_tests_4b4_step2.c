/**
 * @file argus_tests_4b4_step2.c
 * @brief Pure and seam tests for Phase 4B.4 Step 2 HTTP command admission.
 */

#include "argus_tests_4b4_step2.h"

#include "argus_browser_command_endpoint.h"
#include "argus_http_server.h"

#include <string.h>

#define TEST_CHECK(condition) do { if (!(condition)) return ESP_FAIL; } while (0)
#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

typedef struct {
    argus_net_snapshot_t net;
    argus_authority_snapshot_t authority;
    esp_err_t net_result;
    esp_err_t authority_result;
    esp_err_t dispatch_result;
    int net_calls;
    int authority_calls;
    int dispatch_calls;
    char order[4];
    size_t order_len;
    argus_command_envelope_t envelope;
} endpoint_trace_t;

static void trace_step(endpoint_trace_t *trace, char step)
{
    if (trace->order_len < sizeof(trace->order)) {
        trace->order[trace->order_len++] = step;
    }
}

static esp_err_t mock_get_network(void *ctx, argus_net_snapshot_t *out_snap)
{
    endpoint_trace_t *trace = (endpoint_trace_t *)ctx;
    trace->net_calls++;
    trace_step(trace, 'N');
    if (trace->net_result == ESP_OK) {
        *out_snap = trace->net;
    }
    return trace->net_result;
}

static esp_err_t mock_get_authority(void *ctx, argus_authority_snapshot_t *out_snap)
{
    endpoint_trace_t *trace = (endpoint_trace_t *)ctx;
    trace->authority_calls++;
    trace_step(trace, 'A');
    if (trace->authority_result == ESP_OK) {
        *out_snap = trace->authority;
    }
    return trace->authority_result;
}

static esp_err_t mock_dispatch(void *ctx, const argus_command_envelope_t *envelope)
{
    endpoint_trace_t *trace = (endpoint_trace_t *)ctx;
    trace->dispatch_calls++;
    trace_step(trace, 'D');
    trace->envelope = *envelope;
    return trace->dispatch_result;
}

static void init_admitted_trace(endpoint_trace_t *trace)
{
    memset(trace, 0, sizeof(*trace));
    trace->net.mode = ARGUS_NET_MODE_SERVICE_AP_ONLY;
    trace->authority.mode = ARGUS_AUTHORITY_LOCAL_SERVICE;
    trace->authority.owner = ARGUS_AUTH_OWNER_BROWSER;
    trace->authority.generation = 73U;
    trace->net_result = ESP_OK;
    trace->authority_result = ESP_OK;
    trace->dispatch_result = ESP_OK;
}

static argus_browser_command_endpoint_ops_t make_ops(endpoint_trace_t *trace)
{
    const argus_browser_command_endpoint_ops_t ops = {
        .get_network_snapshot = mock_get_network,
        .get_authority_snapshot = mock_get_authority,
        .dispatch = mock_dispatch,
        .ctx = trace,
    };
    return ops;
}

static argus_browser_command_endpoint_result_t process_json(
    bool authenticated,
    const char *json,
    endpoint_trace_t *trace,
    argus_browser_command_endpoint_outcome_t *outcome)
{
    argus_browser_command_endpoint_ops_t ops = make_ops(trace);
    return argus_browser_command_endpoint_process(
        authenticated, (const uint8_t *)json, strlen(json), &ops, outcome);
}

typedef struct {
    const uint8_t *data;
    size_t data_len;
    size_t offset;
    size_t max_chunk;
    int terminal_result;
    int calls;
} receive_trace_t;

static int mock_receive(void *ctx, uint8_t *dst, size_t max_len)
{
    receive_trace_t *trace = (receive_trace_t *)ctx;
    trace->calls++;
    if (trace->offset >= trace->data_len) {
        return trace->terminal_result;
    }
    size_t chunk = trace->data_len - trace->offset;
    if (chunk > max_len) chunk = max_len;
    if (trace->max_chunk > 0U && chunk > trace->max_chunk) chunk = trace->max_chunk;
    memcpy(dst, trace->data + trace->offset, chunk);
    trace->offset += chunk;
    return (int)chunk;
}

esp_err_t test_4b4_endpoint_registration_contract(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    TEST_CHECK(argus_http_test_command_registration());
#endif
    TEST_CHECK(strcmp(ARGUS_BROWSER_COMMAND_URI, "/api/command") == 0);
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_body_receive_success(void)
{
    uint8_t source[ARGUS_BROWSER_COMMAND_MAX_BODY_LEN];
    uint8_t output[ARGUS_BROWSER_COMMAND_MAX_BODY_LEN];
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)i;

    receive_trace_t trace = {
        .data = source,
        .data_len = sizeof(source),
        .max_chunk = 17U,
    };
    size_t received = 99U;
    TEST_CHECK(argus_browser_command_receive_body(sizeof(source), mock_receive, &trace,
                                                   output, sizeof(output), &received) ==
               ARGUS_BROWSER_BODY_RECEIVE_OK);
    TEST_CHECK(received == sizeof(source));
    TEST_CHECK(trace.calls > 1);
    TEST_CHECK(memcmp(source, output, sizeof(source)) == 0);

    static const uint8_t short_body[] = {'{', '}', 0, 'x'};
    trace = (receive_trace_t){
        .data = short_body,
        .data_len = sizeof(short_body),
        .max_chunk = 1U,
    };
    TEST_CHECK(argus_browser_command_receive_body(sizeof(short_body), mock_receive, &trace,
                                                   output, sizeof(output), &received) ==
               ARGUS_BROWSER_BODY_RECEIVE_OK);
    TEST_CHECK(received == sizeof(short_body));
    TEST_CHECK(memcmp(short_body, output, sizeof(short_body)) == 0);
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_body_receive_failures(void)
{
    uint8_t output[ARGUS_BROWSER_COMMAND_MAX_BODY_LEN];
    static const uint8_t source[] = {'a', 'b'};
    size_t received = 99U;
    receive_trace_t trace = { .data = source, .data_len = sizeof(source) };

    TEST_CHECK(argus_browser_command_receive_body(0U, mock_receive, &trace,
                                                   output, sizeof(output), &received) ==
               ARGUS_BROWSER_BODY_RECEIVE_EMPTY);
    TEST_CHECK(received == 0U && trace.calls == 0);
    TEST_CHECK(argus_browser_command_receive_body(
                   ARGUS_BROWSER_COMMAND_MAX_BODY_LEN + 1U, mock_receive, &trace,
                   output, sizeof(output), &received) ==
               ARGUS_BROWSER_BODY_RECEIVE_TOO_LARGE);
    TEST_CHECK(received == 0U && trace.calls == 0 && trace.offset == 0U);

    trace = (receive_trace_t){ .data = source, .data_len = sizeof(source),
                               .terminal_result = 0 };
    TEST_CHECK(argus_browser_command_receive_body(3U, mock_receive, &trace,
                                                   output, sizeof(output), &received) ==
               ARGUS_BROWSER_BODY_RECEIVE_TRUNCATED);
    TEST_CHECK(received == 0U);

    trace = (receive_trace_t){ .terminal_result = ARGUS_BROWSER_BODY_RECV_TIMEOUT };
    TEST_CHECK(argus_browser_command_receive_body(1U, mock_receive, &trace,
                                                   output, sizeof(output), &received) ==
               ARGUS_BROWSER_BODY_RECEIVE_TIMEOUT);
    trace = (receive_trace_t){ .terminal_result = -1 };
    TEST_CHECK(argus_browser_command_receive_body(1U, mock_receive, &trace,
                                                   output, sizeof(output), &received) ==
               ARGUS_BROWSER_BODY_RECEIVE_ERROR);
    TEST_CHECK(argus_browser_command_receive_body(1U, NULL, NULL,
                                                   output, sizeof(output), &received) ==
               ARGUS_BROWSER_BODY_RECEIVE_INVALID_ARGUMENT);
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_auth_and_decoder_rejections(void)
{
    endpoint_trace_t trace;
    init_admitted_trace(&trace);
    argus_browser_command_endpoint_outcome_t outcome;
    TEST_CHECK(process_json(false, "{\"command\":\"start\"}", &trace, &outcome) ==
               ARGUS_BROWSER_ENDPOINT_UNAUTHORIZED);
    TEST_CHECK(trace.net_calls == 0 && trace.authority_calls == 0 && trace.dispatch_calls == 0);

    static const uint8_t embedded_nul[] = {
        '{','"','c','o','m','m','a','n','d','"',':','"','s','t','a','r','t','"','}',0,'x'
    };
    argus_browser_command_endpoint_ops_t ops = make_ops(&trace);
    TEST_CHECK(argus_browser_command_endpoint_process(true, embedded_nul,
                                                       sizeof(embedded_nul), &ops,
                                                       &outcome) ==
               ARGUS_BROWSER_ENDPOINT_BAD_REQUEST);
    TEST_CHECK(!outcome.envelope_built && trace.dispatch_calls == 0);
    TEST_CHECK(process_json(true, "{\"command\":\"start\"}x", &trace, &outcome) ==
               ARGUS_BROWSER_ENDPOINT_BAD_REQUEST);
    TEST_CHECK(trace.dispatch_calls == 0);
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_admission_matrix(void)
{
    static const struct {
        argus_network_mode_t net_mode;
        argus_control_authority_t auth_mode;
        argus_authority_owner_t owner;
        bool admitted;
    } cases[] = {
        {ARGUS_NET_MODE_SERVICE_AP_ONLY, ARGUS_AUTHORITY_LOCAL_SERVICE,
         ARGUS_AUTH_OWNER_BROWSER, true},
        {ARGUS_NET_MODE_AP_DISCOVERABLE, ARGUS_AUTHORITY_LOCAL_SERVICE,
         ARGUS_AUTH_OWNER_BROWSER, false},
        {ARGUS_NET_MODE_COMMISSIONED_STA, ARGUS_AUTHORITY_LOCAL_SERVICE,
         ARGUS_AUTH_OWNER_BROWSER, false},
        {ARGUS_NET_MODE_SERVICE_AP_ONLY, ARGUS_AUTHORITY_NONE,
         ARGUS_AUTH_OWNER_NONE, false},
        {ARGUS_NET_MODE_SERVICE_AP_ONLY, ARGUS_AUTHORITY_SUPERVISORY,
         ARGUS_AUTH_OWNER_MQTT, false},
        {ARGUS_NET_MODE_SERVICE_AP_ONLY, ARGUS_AUTHORITY_SERVICE_TRANSITION,
         ARGUS_AUTH_OWNER_NONE, false},
        {ARGUS_NET_MODE_SERVICE_AP_ONLY, ARGUS_AUTHORITY_LOCAL_SERVICE,
         ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI, false},
        {ARGUS_NET_MODE_SERVICE_AP_ONLY, ARGUS_AUTHORITY_LOCAL_SERVICE,
         ARGUS_AUTH_OWNER_NONE, false},
    };

    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        endpoint_trace_t trace;
        init_admitted_trace(&trace);
        trace.net.mode = cases[i].net_mode;
        trace.authority.mode = cases[i].auth_mode;
        trace.authority.owner = cases[i].owner;
        argus_browser_command_endpoint_outcome_t outcome;
        argus_browser_command_endpoint_result_t result =
            process_json(true, "{\"command\":\"estop\"}", &trace, &outcome);
        TEST_CHECK(result == (cases[i].admitted ? ARGUS_BROWSER_ENDPOINT_OK
                                                : ARGUS_BROWSER_ENDPOINT_FORBIDDEN));
        TEST_CHECK(trace.dispatch_calls == (cases[i].admitted ? 1 : 0));
        if (cases[i].net_mode != ARGUS_NET_MODE_SERVICE_AP_ONLY) {
            TEST_CHECK(trace.authority_calls == 0);
        }
    }
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_argument_free_envelopes(void)
{
    static const struct {
        const char *json;
        argus_cmd_type_t type;
    } cases[] = {
        {"{\"command\":\"start\"}", ARGUS_CMD_TYPE_START},
        {"{\"command\":\"stop\"}", ARGUS_CMD_TYPE_STOP_NORMAL},
        {"{\"command\":\"unlock\"}", ARGUS_CMD_TYPE_UNLOCK},
        {"{\"command\":\"estop\"}", ARGUS_CMD_TYPE_ESTOP},
        {"{\"command\":\"reset_estop\"}", ARGUS_CMD_TYPE_RESET_ESTOP},
        {"{\"command\":\"recover\"}", ARGUS_CMD_TYPE_RECOVER},
    };

    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        endpoint_trace_t trace;
        init_admitted_trace(&trace);
        memset(&trace.envelope, 0xA5, sizeof(trace.envelope));
        argus_browser_command_endpoint_outcome_t outcome;
        TEST_CHECK(process_json(true, cases[i].json, &trace, &outcome) ==
                   ARGUS_BROWSER_ENDPOINT_OK);
        TEST_CHECK(trace.dispatch_calls == 1);
        TEST_CHECK(trace.envelope.source == ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL);
        TEST_CHECK(trace.envelope.command_type == cases[i].type);
        TEST_CHECK(trace.envelope.authority_generation == 73U);
        TEST_CHECK(trace.envelope.target_rpm_milli == 0);
        TEST_CHECK(!trace.envelope.forward);
    }
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_set_target_envelope(void)
{
    endpoint_trace_t trace;
    init_admitted_trace(&trace);
    argus_browser_command_endpoint_outcome_t outcome;
    TEST_CHECK(process_json(true,
                            "{\"command\":\"set_target\",\"target_rpm_milli\":123456,\"forward\":true}",
                            &trace, &outcome) == ARGUS_BROWSER_ENDPOINT_OK);
    TEST_CHECK(trace.dispatch_calls == 1);
    TEST_CHECK(trace.envelope.source == ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL);
    TEST_CHECK(trace.envelope.command_type == ARGUS_CMD_TYPE_SET_TARGET);
    TEST_CHECK(trace.envelope.authority_generation == 73U);
    TEST_CHECK(trace.envelope.target_rpm_milli == 123456);
    TEST_CHECK(trace.envelope.forward);
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_generation_capture_order(void)
{
    endpoint_trace_t trace;
    init_admitted_trace(&trace);
    trace.authority.generation = UINT32_MAX;
    argus_browser_command_endpoint_outcome_t outcome;
    TEST_CHECK(process_json(true, "{\"command\":\"start\"}", &trace, &outcome) ==
               ARGUS_BROWSER_ENDPOINT_OK);
    TEST_CHECK(trace.order_len == 3U);
    TEST_CHECK(memcmp(trace.order, "NAD", 3U) == 0);
    TEST_CHECK(trace.envelope.authority_generation == UINT32_MAX);
    TEST_CHECK(outcome.envelope_built);
    TEST_CHECK(outcome.envelope.source == trace.envelope.source);
    TEST_CHECK(outcome.envelope.command_type == trace.envelope.command_type);
    TEST_CHECK(outcome.envelope.authority_generation ==
               trace.envelope.authority_generation);
    TEST_CHECK(outcome.envelope.target_rpm_milli == trace.envelope.target_rpm_milli);
    TEST_CHECK(outcome.envelope.forward == trace.envelope.forward);
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_dispatch_result_mapping(void)
{
    static const struct {
        esp_err_t dispatch_result;
        argus_browser_command_endpoint_result_t endpoint_result;
    } cases[] = {
        {ESP_OK, ARGUS_BROWSER_ENDPOINT_OK},
        {ESP_ERR_INVALID_STATE, ARGUS_BROWSER_ENDPOINT_CONFLICT},
        {ESP_ERR_INVALID_ARG, ARGUS_BROWSER_ENDPOINT_CONFLICT},
        {ESP_FAIL, ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR},
        {ESP_ERR_NO_MEM, ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR},
    };

    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        endpoint_trace_t trace;
        init_admitted_trace(&trace);
        trace.dispatch_result = cases[i].dispatch_result;
        argus_browser_command_endpoint_outcome_t outcome;
        TEST_CHECK(process_json(true, "{\"command\":\"start\"}", &trace, &outcome) ==
                   cases[i].endpoint_result);
        TEST_CHECK(trace.dispatch_calls == 1);
        TEST_CHECK(outcome.operation_error == cases[i].dispatch_result);
    }
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_response_contract(void)
{
    static const struct {
        argus_browser_command_endpoint_result_t result;
        int status;
    } cases[] = {
        {ARGUS_BROWSER_ENDPOINT_OK, 200},
        {ARGUS_BROWSER_ENDPOINT_BAD_REQUEST, 400},
        {ARGUS_BROWSER_ENDPOINT_UNAUTHORIZED, 401},
        {ARGUS_BROWSER_ENDPOINT_FORBIDDEN, 403},
        {ARGUS_BROWSER_ENDPOINT_CONFLICT, 409},
        {ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR, 500},
    };

    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        argus_browser_command_http_response_t response;
        TEST_CHECK(argus_browser_command_response_for(cases[i].result, &response));
        TEST_CHECK(response.status_code == cases[i].status);
        TEST_CHECK(response.status_line != NULL && response.json_body != NULL);
        TEST_CHECK(response.json_body[0] == '{');
        TEST_CHECK(strlen(response.json_body) < 96U);
        TEST_CHECK(strcmp(response.cache_control, "no-store") == 0);
    }
    argus_browser_command_http_response_t response;
    TEST_CHECK(!argus_browser_command_response_for(
        (argus_browser_command_endpoint_result_t)99, &response));
    TEST_CHECK(!argus_browser_command_response_for(ARGUS_BROWSER_ENDPOINT_OK, NULL));
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_routing_field_rejections(void)
{
    static const char *cases[] = {
        "{\"command\":\"start\",\"source\":\"portal\"}",
        "{\"command\":\"start\",\"authority_generation\":73}",
        "{\"command\":\"start\",\"authority_mode\":3}",
        "{\"command\":\"start\",\"authority_owner\":2}",
        "{\"command\":\"start\",\"pump_id\":1}",
        "{\"command\":\"start\",\"channel_id\":1}",
        "{\"command\":\"start\",\"axis_id\":1}",
        "{\"command\":\"start\",\"routing_destination\":\"local\"}",
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        endpoint_trace_t trace;
        init_admitted_trace(&trace);
        argus_browser_command_endpoint_outcome_t outcome;
        TEST_CHECK(process_json(true, cases[i], &trace, &outcome) ==
                   ARGUS_BROWSER_ENDPOINT_BAD_REQUEST);
        TEST_CHECK(trace.net_calls == 0 && trace.authority_calls == 0 &&
                   trace.dispatch_calls == 0 && !outcome.envelope_built);
    }
    return ESP_OK;
}

esp_err_t test_4b4_endpoint_invalid_ops_are_isolated(void)
{
    static const char json[] = "{\"command\":\"start\"}";
    endpoint_trace_t trace;
    init_admitted_trace(&trace);
    argus_browser_command_endpoint_outcome_t outcome;

    TEST_CHECK(argus_browser_command_endpoint_process(
                   true, (const uint8_t *)json, sizeof(json) - 1U, NULL, &outcome) ==
               ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR);
    TEST_CHECK(!outcome.envelope_built);

    argus_browser_command_endpoint_ops_t ops = make_ops(&trace);
    trace.net_result = ESP_FAIL;
    TEST_CHECK(argus_browser_command_endpoint_process(
                   true, (const uint8_t *)json, sizeof(json) - 1U, &ops, &outcome) ==
               ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR);
    TEST_CHECK(trace.authority_calls == 0 && trace.dispatch_calls == 0);

    init_admitted_trace(&trace);
    trace.authority_result = ESP_FAIL;
    ops = make_ops(&trace);
    TEST_CHECK(argus_browser_command_endpoint_process(
                   true, (const uint8_t *)json, sizeof(json) - 1U, &ops, &outcome) ==
               ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR);
    TEST_CHECK(trace.net_calls == 1 && trace.authority_calls == 1 && trace.dispatch_calls == 0);
    TEST_CHECK(!outcome.envelope_built);
    return ESP_OK;
}
