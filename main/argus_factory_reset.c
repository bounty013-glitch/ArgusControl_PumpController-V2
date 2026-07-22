/**
 * @file argus_factory_reset.c
 * @brief Strict request validation and production-used reset orchestration.
 */

#include "argus_factory_reset.h"

#include <ctype.h>
#include <string.h>

typedef struct {
    const uint8_t *body;
    size_t len;
    size_t pos;
} reset_parser_t;

static bool is_ws(uint8_t ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void skip_ws(reset_parser_t *p)
{
    while (p->pos < p->len && is_ws(p->body[p->pos])) p->pos++;
}

static bool parse_ascii_string(reset_parser_t *p, char *out, size_t out_size)
{
    if (p->pos >= p->len || p->body[p->pos] != '"' || out_size == 0U) {
        return false;
    }
    p->pos++;
    size_t used = 0U;
    while (p->pos < p->len) {
        uint8_t ch = p->body[p->pos++];
        if (ch == '"') {
            out[used] = '\0';
            return true;
        }
        if (ch < 0x20U || ch > 0x7EU || ch == '\\' || used + 1U >= out_size) {
            return false;
        }
        out[used++] = (char)ch;
    }
    return false;
}

argus_factory_reset_decode_result_t argus_factory_reset_decode(
    const uint8_t *body, size_t body_len)
{
    if (body == NULL && body_len != 0U) {
        return ARGUS_FACTORY_RESET_DECODE_INVALID_ARGUMENT;
    }
    if (body_len == 0U) return ARGUS_FACTORY_RESET_DECODE_EMPTY;
    if (body_len > ARGUS_FACTORY_RESET_MAX_BODY_LEN) {
        return ARGUS_FACTORY_RESET_DECODE_TOO_LARGE;
    }

    reset_parser_t p = {.body = body, .len = body_len, .pos = 0U};
    skip_ws(&p);
    if (p.pos >= p.len || p.body[p.pos++] != '{') {
        return ARGUS_FACTORY_RESET_DECODE_MALFORMED;
    }
    skip_ws(&p);
    if (p.pos < p.len && p.body[p.pos] == '}') {
        return ARGUS_FACTORY_RESET_DECODE_MISSING_CONFIRMATION;
    }

    char key[16];
    if (!parse_ascii_string(&p, key, sizeof(key))) {
        return ARGUS_FACTORY_RESET_DECODE_MALFORMED;
    }
    if (strcmp(key, "confirm") != 0) {
        return ARGUS_FACTORY_RESET_DECODE_UNKNOWN_FIELD;
    }
    skip_ws(&p);
    if (p.pos >= p.len || p.body[p.pos++] != ':') {
        return ARGUS_FACTORY_RESET_DECODE_MALFORMED;
    }
    skip_ws(&p);
    char confirmation[24];
    if (!parse_ascii_string(&p, confirmation, sizeof(confirmation))) {
        return ARGUS_FACTORY_RESET_DECODE_MALFORMED;
    }
    if (strcmp(confirmation, ARGUS_FACTORY_RESET_CONFIRMATION) != 0) {
        return ARGUS_FACTORY_RESET_DECODE_WRONG_CONFIRMATION;
    }
    skip_ws(&p);
    if (p.pos < p.len && p.body[p.pos] == ',') {
        p.pos++;
        skip_ws(&p);
        char second_key[16];
        if (!parse_ascii_string(&p, second_key, sizeof(second_key))) {
            return ARGUS_FACTORY_RESET_DECODE_MALFORMED;
        }
        return strcmp(second_key, "confirm") == 0
                   ? ARGUS_FACTORY_RESET_DECODE_DUPLICATE_FIELD
                   : ARGUS_FACTORY_RESET_DECODE_UNKNOWN_FIELD;
    }
    if (p.pos >= p.len || p.body[p.pos++] != '}') {
        return ARGUS_FACTORY_RESET_DECODE_MALFORMED;
    }
    skip_ws(&p);
    return p.pos == p.len ? ARGUS_FACTORY_RESET_DECODE_OK
                          : ARGUS_FACTORY_RESET_DECODE_MALFORMED;
}

bool argus_factory_reset_content_type_valid(const char *content_type)
{
    if (content_type == NULL) return false;
    static const char expected[] = "application/json";
    size_t i = 0U;
    for (; i < sizeof(expected) - 1U; i++) {
        if (content_type[i] == '\0' ||
            tolower((unsigned char)content_type[i]) != expected[i]) {
            return false;
        }
    }
    while (content_type[i] == ' ' || content_type[i] == '\t') i++;
    if (content_type[i] == '\0') return true;
    if (content_type[i++] != ';') return false;
    while (content_type[i] == ' ' || content_type[i] == '\t') i++;
    static const char charset[] = "charset=utf-8";
    for (size_t j = 0U; j < sizeof(charset) - 1U; j++, i++) {
        if (content_type[i] == '\0' ||
            tolower((unsigned char)content_type[i]) != charset[j]) {
            return false;
        }
    }
    while (content_type[i] == ' ' || content_type[i] == '\t') i++;
    return content_type[i] == '\0';
}

argus_factory_reset_receive_result_t argus_factory_reset_receive_body(
    size_t content_len, argus_factory_reset_recv_fn_t recv_fn, void *recv_ctx,
    uint8_t *out_body, size_t out_capacity, size_t *out_len)
{
    if (out_len != NULL) *out_len = 0U;
    if (recv_fn == NULL || out_body == NULL || out_len == NULL) {
        return ARGUS_FACTORY_RESET_RECEIVE_INVALID_ARGUMENT;
    }
    if (content_len == 0U) return ARGUS_FACTORY_RESET_RECEIVE_EMPTY;
    if (content_len > ARGUS_FACTORY_RESET_MAX_BODY_LEN) {
        return ARGUS_FACTORY_RESET_RECEIVE_TOO_LARGE;
    }
    if (content_len > out_capacity) {
        return ARGUS_FACTORY_RESET_RECEIVE_INVALID_ARGUMENT;
    }
    size_t received = 0U;
    while (received < content_len) {
        int chunk = recv_fn(recv_ctx, out_body + received, content_len - received);
        if (chunk == ARGUS_FACTORY_RESET_BODY_RECV_TIMEOUT) {
            return ARGUS_FACTORY_RESET_RECEIVE_TIMEOUT;
        }
        if (chunk == 0) return ARGUS_FACTORY_RESET_RECEIVE_TRUNCATED;
        if (chunk < 0 || (size_t)chunk > content_len - received) {
            return ARGUS_FACTORY_RESET_RECEIVE_ERROR;
        }
        received += (size_t)chunk;
    }
    *out_len = received;
    return ARGUS_FACTORY_RESET_RECEIVE_OK;
}

bool argus_factory_reset_receive_disposition(
    argus_factory_reset_receive_result_t result,
    argus_factory_reset_receive_disposition_t *out)
{
    if (out == NULL) return false;
    *out = (argus_factory_reset_receive_disposition_t){
        .continue_request = false,
        .close_session = true,
        .status_code = 500,
        .handler_result_after_response = ESP_FAIL,
    };
    switch (result) {
        case ARGUS_FACTORY_RESET_RECEIVE_OK:
            out->continue_request = true;
            out->close_session = false;
            out->status_code = 202;
            out->handler_result_after_response = ESP_OK;
            return true;
        case ARGUS_FACTORY_RESET_RECEIVE_EMPTY:
            out->close_session = false;
            out->status_code = 400;
            out->handler_result_after_response = ESP_OK;
            return true;
        case ARGUS_FACTORY_RESET_RECEIVE_TOO_LARGE:
        case ARGUS_FACTORY_RESET_RECEIVE_TRUNCATED:
        case ARGUS_FACTORY_RESET_RECEIVE_TIMEOUT:
        case ARGUS_FACTORY_RESET_RECEIVE_ERROR:
            out->status_code = 400;
            return true;
        case ARGUS_FACTORY_RESET_RECEIVE_INVALID_ARGUMENT:
        default:
            return true;
    }
}

static bool state_is_stationary_safe(const argus_state_snapshot_t *state)
{
    return state != NULL && !state->estop_latched && state->fault_code == 0U &&
           (state->machine_state == ARGUS_STATE_UNLOCKED ||
            state->machine_state == ARGUS_STATE_HOLDING) &&
           state->applied_rpm_milli == 0 && state->generated_rpm_milli == 0 &&
           !state->ramp_active;
}

static bool service_network_is_converged(const argus_net_snapshot_t *net)
{
    return net != NULL && !net->sta_connected && !net->sta_ip_acquired &&
           net->sta_state == ARGUS_STA_DISABLED && net->ap_started &&
           !net->mqtt_broker_running && net->mqtt_broker_stopped;
}

argus_factory_reset_policy_result_t argus_factory_reset_evaluate_policy(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *authority,
    const argus_state_snapshot_t *state,
    bool lifecycle_conflict)
{
    if (net == NULL || authority == NULL || state == NULL) {
        return ARGUS_FACTORY_RESET_POLICY_LIFECYCLE_CONFLICT;
    }
    if (lifecycle_conflict || net->wifi_transaction_active) {
        return ARGUS_FACTORY_RESET_POLICY_LIFECYCLE_CONFLICT;
    }
    if (net->mode != ARGUS_NET_MODE_SERVICE_AP_ONLY) {
        return ARGUS_FACTORY_RESET_POLICY_WRONG_NETWORK;
    }
    if (authority->mode != ARGUS_AUTHORITY_LOCAL_SERVICE ||
        authority->owner != ARGUS_AUTH_OWNER_BROWSER) {
        return ARGUS_FACTORY_RESET_POLICY_WRONG_AUTHORITY;
    }
    if (!state_is_stationary_safe(state)) {
        return ARGUS_FACTORY_RESET_POLICY_UNSAFE_MACHINE;
    }
    if (!service_network_is_converged(net)) {
        return ARGUS_FACTORY_RESET_POLICY_NETWORK_NOT_CONVERGED;
    }
    return ARGUS_FACTORY_RESET_POLICY_OK;
}

static bool ops_valid(const argus_factory_reset_ops_t *ops)
{
    return ops != NULL && ops->lock_lifecycle != NULL &&
           ops->unlock_lifecycle != NULL && ops->lock_dispatch != NULL &&
           ops->unlock_dispatch != NULL && ops->get_network_snapshot != NULL &&
           ops->get_authority_snapshot != NULL &&
           ops->get_state_snapshot != NULL && ops->begin_transition != NULL &&
           ops->response_grace_delay != NULL && ops->stop_http != NULL &&
           ops->revoke_authority != NULL &&
           ops->erase_configuration != NULL && ops->reboot != NULL;
}

argus_factory_reset_result_t argus_factory_reset_execute(
    const argus_factory_reset_ops_t *ops)
{
    argus_factory_reset_result_t result = {.error = ESP_OK};
    if (!ops_valid(ops)) {
        result.failed_at_step = ARGUS_FACTORY_RESET_STEP_PREFLIGHT;
        result.error = ESP_ERR_INVALID_ARG;
        return result;
    }

    result.failed_at_step = ARGUS_FACTORY_RESET_STEP_LOCK_LIFECYCLE;
    result.error = ops->lock_lifecycle(ops->ctx);
    if (result.error != ESP_OK) return result;
    ops->lock_dispatch(ops->ctx);

    argus_net_snapshot_t net;
    argus_authority_snapshot_t authority;
    argus_state_snapshot_t state;
    result.failed_at_step = ARGUS_FACTORY_RESET_STEP_PREFLIGHT;
    result.error = ops->get_network_snapshot(ops->ctx, &net);
    if (result.error == ESP_OK) {
        result.error = ops->get_authority_snapshot(ops->ctx, &authority);
    }
    if (result.error == ESP_OK) ops->get_state_snapshot(ops->ctx, &state);
    if (result.error != ESP_OK ||
        argus_factory_reset_evaluate_policy(&net, &authority, &state, false) !=
            ARGUS_FACTORY_RESET_POLICY_OK) {
        if (result.error == ESP_OK) result.error = ESP_ERR_INVALID_STATE;
        ops->unlock_dispatch(ops->ctx);
        ops->unlock_lifecycle(ops->ctx);
        return result;
    }

    result.failed_at_step = ARGUS_FACTORY_RESET_STEP_BEGIN_TRANSITION;
    result.error = ops->begin_transition(ops->ctx);
    ops->unlock_dispatch(ops->ctx);
    ops->unlock_lifecycle(ops->ctx);
    if (result.error != ESP_OK) return result;

    result.failed_at_step = ARGUS_FACTORY_RESET_STEP_RESPONSE_GRACE;
    ops->response_grace_delay(ops->ctx);
    result.failed_at_step = ARGUS_FACTORY_RESET_STEP_STOP_HTTP;
    result.error = ops->stop_http(ops->ctx);
    if (result.error != ESP_OK) {
        if (ops->lock_lifecycle(ops->ctx) == ESP_OK) {
            ops->lock_dispatch(ops->ctx);
            if (ops->revoke_authority(ops->ctx) == ESP_OK) {
                result.authority_revoked = true;
            }
            ops->unlock_dispatch(ops->ctx);
            ops->unlock_lifecycle(ops->ctx);
        }
        return result;
    }
    result.http_stopped = true;

    result.failed_at_step = ARGUS_FACTORY_RESET_STEP_FINAL_REVALIDATION;
    result.error = ops->lock_lifecycle(ops->ctx);
    if (result.error != ESP_OK) return result;
    ops->lock_dispatch(ops->ctx);
    result.error = ops->get_network_snapshot(ops->ctx, &net);
    if (result.error == ESP_OK) {
        result.error = ops->get_authority_snapshot(ops->ctx, &authority);
    }
    if (result.error == ESP_OK) ops->get_state_snapshot(ops->ctx, &state);
    bool transition_valid = result.error == ESP_OK &&
        net.mode == ARGUS_NET_MODE_SERVICE_TRANSITION &&
        service_network_is_converged(&net) &&
        authority.mode == ARGUS_AUTHORITY_SERVICE_TRANSITION &&
        authority.owner == ARGUS_AUTH_OWNER_NONE &&
        state_is_stationary_safe(&state);
    if (!transition_valid) {
        if (result.error == ESP_OK) result.error = ESP_ERR_INVALID_STATE;
        (void)ops->revoke_authority(ops->ctx);
        ops->unlock_dispatch(ops->ctx);
        ops->unlock_lifecycle(ops->ctx);
        return result;
    }

    result.failed_at_step = ARGUS_FACTORY_RESET_STEP_REVOKE_AUTHORITY;
    result.error = ops->revoke_authority(ops->ctx);
    if (result.error == ESP_OK) result.authority_revoked = true;
    ops->unlock_dispatch(ops->ctx);
    ops->unlock_lifecycle(ops->ctx);
    if (result.error != ESP_OK) return result;

    result.failed_at_step = ARGUS_FACTORY_RESET_STEP_ERASE_CONFIG;
    result.error = ops->erase_configuration(ops->ctx);
    if (result.error != ESP_OK) return result;
    result.configuration_erased = true;

    result.failed_at_step = ARGUS_FACTORY_RESET_STEP_REBOOT;
    result.accepted = true;
    result.reboot_called = true;
    result.failed_at_step = 0;
    ops->reboot(ops->ctx);
    return result;
}
