/**
 * @file argus_tests_4b6.c
 * @brief Production-used pure contracts for Phase 4B.6 portal lifecycle.
 */

#include "argus_tests_4b6.h"

#include <string.h>

#include "argus_factory_reset.h"
#include "argus_http_server.h"
#include "argus_nvs_config.h"

#define CHECK(condition) do { if (!(condition)) return ESP_FAIL; } while (0)

esp_err_t test_4b6_factory_reset_decoder_acceptance(void)
{
    static const char exact[] = "{\"confirm\":\"FACTORY_RESET\"}";
    static const char spaced[] =
        " \r\n { \"confirm\" : \"FACTORY_RESET\" } \t";
    CHECK(argus_factory_reset_decode((const uint8_t *)exact,
                                     sizeof(exact) - 1U) ==
          ARGUS_FACTORY_RESET_DECODE_OK);
    CHECK(argus_factory_reset_decode((const uint8_t *)spaced,
                                     sizeof(spaced) - 1U) ==
          ARGUS_FACTORY_RESET_DECODE_OK);
    return ESP_OK;
}

esp_err_t test_4b6_factory_reset_decoder_rejections(void)
{
    static const char *const rejected[] = {
        "", "{}", "{\"confirm\":", "[]",
        "{\"confirm\":true}",
        "{\"confirm\":\"NO\"}",
        "{\"confirm\":\"FACTORY_RESET\",\"confirm\":\"FACTORY_RESET\"}",
        "{\"confirm\":\"FACTORY_RESET\",\"extra\":1}",
        "{\"other\":\"FACTORY_RESET\"}",
        "{\"confirm\":\"FACTORY_RESET\"} trailing",
    };
    for (size_t i = 0U; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        CHECK(argus_factory_reset_decode((const uint8_t *)rejected[i],
                                         strlen(rejected[i])) !=
              ARGUS_FACTORY_RESET_DECODE_OK);
    }
    uint8_t oversized[ARGUS_FACTORY_RESET_MAX_BODY_LEN + 1U] = {'{'};
    CHECK(argus_factory_reset_decode(oversized, sizeof(oversized)) ==
          ARGUS_FACTORY_RESET_DECODE_TOO_LARGE);
    CHECK(argus_factory_reset_decode(NULL, 1U) ==
          ARGUS_FACTORY_RESET_DECODE_INVALID_ARGUMENT);
    return ESP_OK;
}

esp_err_t test_4b6_factory_reset_content_type_contract(void)
{
    CHECK(argus_factory_reset_content_type_valid("application/json"));
    CHECK(argus_factory_reset_content_type_valid("Application/JSON"));
    CHECK(argus_factory_reset_content_type_valid(
        "application/json; charset=utf-8"));
    CHECK(!argus_factory_reset_content_type_valid(NULL));
    CHECK(!argus_factory_reset_content_type_valid("text/plain"));
    CHECK(!argus_factory_reset_content_type_valid("application/jsonp"));
    CHECK(!argus_factory_reset_content_type_valid(
        "application/json; charset=latin1"));
    return ESP_OK;
}

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    size_t max_chunk;
    int terminal;
    int calls;
} recv_mock_t;

static int mock_recv(void *ctx, uint8_t *dst, size_t max_len)
{
    recv_mock_t *mock = (recv_mock_t *)ctx;
    mock->calls++;
    if (mock->pos == mock->len) return mock->terminal;
    size_t available = mock->len - mock->pos;
    size_t chunk = available < max_len ? available : max_len;
    if (mock->max_chunk != 0U && chunk > mock->max_chunk) chunk = mock->max_chunk;
    memcpy(dst, mock->data + mock->pos, chunk);
    mock->pos += chunk;
    return (int)chunk;
}

esp_err_t test_4b6_factory_reset_receive_contract(void)
{
    static const uint8_t body[] = "{\"confirm\":\"FACTORY_RESET\"}";
    recv_mock_t mock = {.data = body, .len = sizeof(body) - 1U, .max_chunk = 3U};
    uint8_t received[ARGUS_FACTORY_RESET_MAX_BODY_LEN];
    size_t received_len = 99U;
    CHECK(argus_factory_reset_receive_body(mock.len, mock_recv, &mock, received,
                                            sizeof(received), &received_len) ==
          ARGUS_FACTORY_RESET_RECEIVE_OK);
    CHECK(received_len == mock.len && memcmp(received, body, mock.len) == 0);
    CHECK(mock.calls > 1);

    received_len = 99U;
    CHECK(argus_factory_reset_receive_body(0U, mock_recv, &mock, received,
                                            sizeof(received), &received_len) ==
          ARGUS_FACTORY_RESET_RECEIVE_EMPTY);
    CHECK(received_len == 0U);
    recv_mock_t no_progress = {.terminal = 0};
    CHECK(argus_factory_reset_receive_body(1U, mock_recv, &no_progress,
                                            received, sizeof(received),
                                            &received_len) ==
          ARGUS_FACTORY_RESET_RECEIVE_TRUNCATED);
    recv_mock_t timeout = {.terminal = ARGUS_FACTORY_RESET_BODY_RECV_TIMEOUT};
    CHECK(argus_factory_reset_receive_body(1U, mock_recv, &timeout, received,
                                            sizeof(received), &received_len) ==
          ARGUS_FACTORY_RESET_RECEIVE_TIMEOUT);
    recv_mock_t error = {.terminal = -7};
    CHECK(argus_factory_reset_receive_body(1U, mock_recv, &error, received,
                                            sizeof(received), &received_len) ==
          ARGUS_FACTORY_RESET_RECEIVE_ERROR);
    return ESP_OK;
}

esp_err_t test_4b6_factory_reset_receive_close_contract(void)
{
    const argus_factory_reset_receive_result_t uncertain[] = {
        ARGUS_FACTORY_RESET_RECEIVE_TOO_LARGE,
        ARGUS_FACTORY_RESET_RECEIVE_TRUNCATED,
        ARGUS_FACTORY_RESET_RECEIVE_TIMEOUT,
        ARGUS_FACTORY_RESET_RECEIVE_ERROR,
    };
    for (size_t i = 0U; i < sizeof(uncertain) / sizeof(uncertain[0]); i++) {
        argus_factory_reset_receive_disposition_t disposition;
        CHECK(argus_factory_reset_receive_disposition(uncertain[i],
                                                       &disposition));
        CHECK(!disposition.continue_request && disposition.close_session);
        CHECK(disposition.status_code == 400);
        CHECK(disposition.handler_result_after_response == ESP_FAIL);
    }
    argus_factory_reset_receive_disposition_t empty;
    CHECK(argus_factory_reset_receive_disposition(
        ARGUS_FACTORY_RESET_RECEIVE_EMPTY, &empty));
    CHECK(!empty.continue_request && !empty.close_session &&
          empty.status_code == 400 && empty.handler_result_after_response == ESP_OK);
    argus_factory_reset_receive_disposition_t success;
    CHECK(argus_factory_reset_receive_disposition(
        ARGUS_FACTORY_RESET_RECEIVE_OK, &success));
    CHECK(success.continue_request && !success.close_session);
    recv_mock_t oversized = {0};
    uint8_t out[ARGUS_FACTORY_RESET_MAX_BODY_LEN];
    size_t out_len;
    CHECK(argus_factory_reset_receive_body(ARGUS_FACTORY_RESET_MAX_BODY_LEN + 1U,
                                            mock_recv, &oversized, out,
                                            sizeof(out), &out_len) ==
          ARGUS_FACTORY_RESET_RECEIVE_TOO_LARGE);
    CHECK(oversized.calls == 0);
    return ESP_OK;
}

static void make_safe_inputs(argus_net_snapshot_t *net,
                             argus_authority_snapshot_t *authority,
                             argus_state_snapshot_t *state)
{
    memset(net, 0, sizeof(*net));
    net->mode = ARGUS_NET_MODE_SERVICE_AP_ONLY;
    net->sta_state = ARGUS_STA_DISABLED;
    net->ap_started = true;
    net->mqtt_broker_stopped = true;
    net->mqtt_broker_observable = true;
    authority->mode = ARGUS_AUTHORITY_LOCAL_SERVICE;
    authority->owner = ARGUS_AUTH_OWNER_BROWSER;
    authority->generation = 7U;
    memset(state, 0, sizeof(*state));
    state->machine_state = ARGUS_STATE_UNLOCKED;
}

esp_err_t test_4b6_factory_reset_policy_matrix(void)
{
    CHECK(argus_nvs_config_namespace_in_factory_reset_scope("argus_cfg"));
    CHECK(argus_nvs_config_namespace_in_factory_reset_scope("argus_sys"));
    CHECK(!argus_nvs_config_namespace_in_factory_reset_scope("argus_rst"));
    CHECK(!argus_nvs_config_namespace_in_factory_reset_scope("argus_portal"));
    CHECK(!argus_nvs_config_namespace_in_factory_reset_scope(NULL));

    argus_net_snapshot_t net;
    argus_authority_snapshot_t authority;
    argus_state_snapshot_t state;
    make_safe_inputs(&net, &authority, &state);
    CHECK(argus_factory_reset_evaluate_policy(&net, &authority, &state, false) ==
          ARGUS_FACTORY_RESET_POLICY_OK);
    CHECK(argus_factory_reset_evaluate_policy(&net, &authority, &state, true) ==
          ARGUS_FACTORY_RESET_POLICY_LIFECYCLE_CONFLICT);
    net.mode = ARGUS_NET_MODE_AP_DISCOVERABLE;
    CHECK(argus_factory_reset_evaluate_policy(&net, &authority, &state, false) ==
          ARGUS_FACTORY_RESET_POLICY_WRONG_NETWORK);
    make_safe_inputs(&net, &authority, &state);
    authority.owner = ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI;
    CHECK(argus_factory_reset_evaluate_policy(&net, &authority, &state, false) ==
          ARGUS_FACTORY_RESET_POLICY_WRONG_AUTHORITY);
    make_safe_inputs(&net, &authority, &state);
    state.generated_rpm_milli = 1;
    CHECK(argus_factory_reset_evaluate_policy(&net, &authority, &state, false) ==
          ARGUS_FACTORY_RESET_POLICY_UNSAFE_MACHINE);
    make_safe_inputs(&net, &authority, &state);
    net.mqtt_broker_stopped = false;
    CHECK(argus_factory_reset_evaluate_policy(&net, &authority, &state, false) ==
          ARGUS_FACTORY_RESET_POLICY_NETWORK_NOT_CONVERGED);
    return ESP_OK;
}

typedef struct {
    argus_net_snapshot_t net;
    argus_authority_snapshot_t authority;
    argus_state_snapshot_t state;
    int sequence;
    int transition_at;
    int grace_at;
    int stop_http_at;
    int revoke_at;
    int erase_at;
    int reboot_at;
    int lifecycle_depth;
    int dispatch_depth;
    esp_err_t stop_http_result;
    esp_err_t erase_result;
    bool make_unsafe_during_grace;
} reset_lifecycle_mock_t;

static esp_err_t lc_lock(void *ctx)
{
    ((reset_lifecycle_mock_t *)ctx)->lifecycle_depth++;
    return ESP_OK;
}
static void lc_unlock(void *ctx)
{
    ((reset_lifecycle_mock_t *)ctx)->lifecycle_depth--;
}
static void dispatch_lock(void *ctx)
{
    ((reset_lifecycle_mock_t *)ctx)->dispatch_depth++;
}
static void dispatch_unlock(void *ctx)
{
    ((reset_lifecycle_mock_t *)ctx)->dispatch_depth--;
}
static esp_err_t get_net(void *ctx, argus_net_snapshot_t *out)
{
    *out = ((reset_lifecycle_mock_t *)ctx)->net;
    return ESP_OK;
}
static esp_err_t get_auth(void *ctx, argus_authority_snapshot_t *out)
{
    *out = ((reset_lifecycle_mock_t *)ctx)->authority;
    return ESP_OK;
}
static void get_state(void *ctx, argus_state_snapshot_t *out)
{
    *out = ((reset_lifecycle_mock_t *)ctx)->state;
}
static esp_err_t begin_transition(void *ctx)
{
    reset_lifecycle_mock_t *m = (reset_lifecycle_mock_t *)ctx;
    m->transition_at = ++m->sequence;
    m->net.mode = ARGUS_NET_MODE_SERVICE_TRANSITION;
    m->authority.mode = ARGUS_AUTHORITY_SERVICE_TRANSITION;
    m->authority.owner = ARGUS_AUTH_OWNER_NONE;
    return ESP_OK;
}
static void response_grace(void *ctx)
{
    reset_lifecycle_mock_t *m = (reset_lifecycle_mock_t *)ctx;
    m->grace_at = ++m->sequence;
    if (m->make_unsafe_during_grace) m->state.generated_rpm_milli = 1;
}
static esp_err_t stop_http(void *ctx)
{
    reset_lifecycle_mock_t *m = (reset_lifecycle_mock_t *)ctx;
    m->stop_http_at = ++m->sequence;
    return m->stop_http_result;
}
static esp_err_t revoke(void *ctx)
{
    reset_lifecycle_mock_t *m = (reset_lifecycle_mock_t *)ctx;
    m->revoke_at = ++m->sequence;
    m->authority.mode = ARGUS_AUTHORITY_NONE;
    return ESP_OK;
}
static esp_err_t erase_config(void *ctx)
{
    reset_lifecycle_mock_t *m = (reset_lifecycle_mock_t *)ctx;
    m->erase_at = ++m->sequence;
    return m->erase_result;
}
static void reboot(void *ctx)
{
    reset_lifecycle_mock_t *m = (reset_lifecycle_mock_t *)ctx;
    m->reboot_at = ++m->sequence;
}

static argus_factory_reset_ops_t make_ops(reset_lifecycle_mock_t *mock)
{
    return (argus_factory_reset_ops_t){
        .lock_lifecycle = lc_lock,
        .unlock_lifecycle = lc_unlock,
        .lock_dispatch = dispatch_lock,
        .unlock_dispatch = dispatch_unlock,
        .get_network_snapshot = get_net,
        .get_authority_snapshot = get_auth,
        .get_state_snapshot = get_state,
        .begin_transition = begin_transition,
        .response_grace_delay = response_grace,
        .stop_http = stop_http,
        .revoke_authority = revoke,
        .erase_configuration = erase_config,
        .reboot = reboot,
        .ctx = mock,
    };
}

static void init_lifecycle_mock(reset_lifecycle_mock_t *mock)
{
    memset(mock, 0, sizeof(*mock));
    make_safe_inputs(&mock->net, &mock->authority, &mock->state);
}

esp_err_t test_4b6_factory_reset_orchestration_success(void)
{
    reset_lifecycle_mock_t mock;
    init_lifecycle_mock(&mock);
    argus_factory_reset_ops_t ops = make_ops(&mock);
    argus_factory_reset_result_t result = argus_factory_reset_execute(&ops);
    CHECK(result.accepted && result.http_stopped && result.authority_revoked &&
          result.configuration_erased && result.reboot_called);
    CHECK(mock.transition_at < mock.grace_at && mock.grace_at < mock.stop_http_at &&
          mock.stop_http_at < mock.revoke_at && mock.revoke_at < mock.erase_at &&
          mock.erase_at < mock.reboot_at);
    CHECK(mock.lifecycle_depth == 0 && mock.dispatch_depth == 0);
    return ESP_OK;
}

esp_err_t test_4b6_factory_reset_orchestration_revalidation(void)
{
    reset_lifecycle_mock_t mock;
    init_lifecycle_mock(&mock);
    mock.make_unsafe_during_grace = true;
    argus_factory_reset_ops_t ops = make_ops(&mock);
    argus_factory_reset_result_t result = argus_factory_reset_execute(&ops);
    CHECK(!result.accepted &&
          result.failed_at_step == ARGUS_FACTORY_RESET_STEP_FINAL_REVALIDATION);
    CHECK(mock.stop_http_at > 0 && mock.revoke_at > 0);
    CHECK(mock.erase_at == 0 && mock.reboot_at == 0);
    CHECK(mock.lifecycle_depth == 0 && mock.dispatch_depth == 0);
    return ESP_OK;
}

esp_err_t test_4b6_factory_reset_orchestration_failures(void)
{
    reset_lifecycle_mock_t mock;
    init_lifecycle_mock(&mock);
    mock.stop_http_result = ESP_FAIL;
    argus_factory_reset_ops_t ops = make_ops(&mock);
    argus_factory_reset_result_t result = argus_factory_reset_execute(&ops);
    CHECK(!result.accepted &&
          result.failed_at_step == ARGUS_FACTORY_RESET_STEP_STOP_HTTP);
    CHECK(result.authority_revoked && mock.revoke_at > mock.stop_http_at);
    CHECK(mock.erase_at == 0 && mock.reboot_at == 0);

    init_lifecycle_mock(&mock);
    mock.erase_result = ESP_ERR_NO_MEM;
    ops = make_ops(&mock);
    result = argus_factory_reset_execute(&ops);
    CHECK(!result.accepted &&
          result.failed_at_step == ARGUS_FACTORY_RESET_STEP_ERASE_CONFIG);
    CHECK(result.authority_revoked && result.http_stopped);
    CHECK(mock.reboot_at == 0);

    init_lifecycle_mock(&mock);
    mock.state.estop_latched = true;
    ops = make_ops(&mock);
    result = argus_factory_reset_execute(&ops);
    CHECK(!result.accepted && mock.transition_at == 0 && mock.stop_http_at == 0 &&
          mock.erase_at == 0 && mock.reboot_at == 0);
    return ESP_OK;
}

esp_err_t test_4b6_factory_reset_http_and_ui_contract(void)
{
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    CHECK(argus_http_test_factory_reset_registration());
#endif
    return ESP_OK;
}
