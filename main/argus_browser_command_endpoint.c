/**
 * @file argus_browser_command_endpoint.c
 * @brief Phase 4B.4 browser-command request framing, admission, and dispatch.
 */

#include "argus_browser_command_endpoint.h"

#include <string.h>

argus_browser_body_receive_result_t argus_browser_command_receive_body(
    size_t content_len,
    argus_browser_body_recv_fn_t recv_fn,
    void *recv_ctx,
    uint8_t *out_body,
    size_t out_capacity,
    size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (recv_fn == NULL || out_body == NULL || out_len == NULL) {
        return ARGUS_BROWSER_BODY_RECEIVE_INVALID_ARGUMENT;
    }
    if (content_len == 0U) {
        return ARGUS_BROWSER_BODY_RECEIVE_EMPTY;
    }
    if (content_len > ARGUS_BROWSER_COMMAND_MAX_BODY_LEN) {
        return ARGUS_BROWSER_BODY_RECEIVE_TOO_LARGE;
    }
    if (content_len > out_capacity) {
        return ARGUS_BROWSER_BODY_RECEIVE_INVALID_ARGUMENT;
    }

    size_t received = 0U;
    while (received < content_len) {
        size_t remaining = content_len - received;
        int chunk = recv_fn(recv_ctx, out_body + received, remaining);
        if (chunk == ARGUS_BROWSER_BODY_RECV_TIMEOUT) {
            return ARGUS_BROWSER_BODY_RECEIVE_TIMEOUT;
        }
        if (chunk == 0) {
            return ARGUS_BROWSER_BODY_RECEIVE_TRUNCATED;
        }
        if (chunk < 0 || (size_t)chunk > remaining) {
            return ARGUS_BROWSER_BODY_RECEIVE_ERROR;
        }
        received += (size_t)chunk;
    }

    *out_len = received;
    return ARGUS_BROWSER_BODY_RECEIVE_OK;
}

static void reset_outcome(argus_browser_command_endpoint_outcome_t *outcome)
{
    memset(outcome, 0, sizeof(*outcome));
    outcome->result = ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR;
    outcome->decode_result = ARGUS_BROWSER_CMD_DECODE_INVALID_ARGUMENT;
    outcome->operation_error = ESP_OK;
}

argus_browser_command_endpoint_result_t argus_browser_command_endpoint_process(
    bool authenticated,
    const uint8_t *body,
    size_t body_len,
    const argus_browser_command_endpoint_ops_t *ops,
    argus_browser_command_endpoint_outcome_t *out_outcome)
{
    if (out_outcome == NULL) {
        return ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR;
    }
    reset_outcome(out_outcome);

    if (!authenticated) {
        out_outcome->result = ARGUS_BROWSER_ENDPOINT_UNAUTHORIZED;
        return out_outcome->result;
    }
    if (body == NULL || ops == NULL ||
        ops->get_network_snapshot == NULL ||
        ops->get_authority_snapshot == NULL ||
        ops->dispatch == NULL) {
        return out_outcome->result;
    }

    argus_browser_command_request_t request;
    out_outcome->decode_result = argus_browser_command_decode(body, body_len, &request);
    if (out_outcome->decode_result != ARGUS_BROWSER_CMD_DECODE_OK || !request.is_valid) {
        out_outcome->result = ARGUS_BROWSER_ENDPOINT_BAD_REQUEST;
        return out_outcome->result;
    }

    argus_net_snapshot_t net_snapshot;
    memset(&net_snapshot, 0, sizeof(net_snapshot));
    esp_err_t err = ops->get_network_snapshot(ops->ctx, &net_snapshot);
    if (err != ESP_OK) {
        out_outcome->operation_error = err;
        return out_outcome->result;
    }
    if (net_snapshot.mode != ARGUS_NET_MODE_SERVICE_AP_ONLY) {
        out_outcome->result = ARGUS_BROWSER_ENDPOINT_FORBIDDEN;
        return out_outcome->result;
    }

    argus_authority_snapshot_t authority_snapshot;
    memset(&authority_snapshot, 0, sizeof(authority_snapshot));
    err = ops->get_authority_snapshot(ops->ctx, &authority_snapshot);
    if (err != ESP_OK) {
        out_outcome->operation_error = err;
        return out_outcome->result;
    }
    if (authority_snapshot.mode != ARGUS_AUTHORITY_LOCAL_SERVICE ||
        authority_snapshot.owner != ARGUS_AUTH_OWNER_BROWSER) {
        out_outcome->result = ARGUS_BROWSER_ENDPOINT_FORBIDDEN;
        return out_outcome->result;
    }

    argus_command_envelope_t envelope;
    memset(&envelope, 0, sizeof(envelope));
    envelope.source = ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL;
    envelope.command_type = request.command_type;
    envelope.authority_generation = authority_snapshot.generation;
    envelope.target_rpm_milli = request.target_rpm_milli;
    envelope.forward = request.forward;

    out_outcome->envelope = envelope;
    out_outcome->envelope_built = true;

    err = ops->dispatch(ops->ctx, &envelope);
    out_outcome->operation_error = err;
    if (err == ESP_OK) {
        out_outcome->result = ARGUS_BROWSER_ENDPOINT_OK;
    } else if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_ARG) {
        out_outcome->result = ARGUS_BROWSER_ENDPOINT_CONFLICT;
    } else {
        out_outcome->result = ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR;
    }
    return out_outcome->result;
}

bool argus_browser_command_response_for(
    argus_browser_command_endpoint_result_t result,
    argus_browser_command_http_response_t *out_response)
{
    if (out_response == NULL) {
        return false;
    }

    out_response->cache_control = "no-store";
    switch (result) {
        case ARGUS_BROWSER_ENDPOINT_OK:
            out_response->status_code = 200;
            out_response->status_line = "200 OK";
            out_response->json_body = "{\"ok\":true,\"status\":\"accepted\"}";
            return true;
        case ARGUS_BROWSER_ENDPOINT_UNAUTHORIZED:
            out_response->status_code = 401;
            out_response->status_line = "401 Unauthorized";
            out_response->json_body = "{\"ok\":false,\"error\":\"unauthorized\"}";
            return true;
        case ARGUS_BROWSER_ENDPOINT_BAD_REQUEST:
            out_response->status_code = 400;
            out_response->status_line = "400 Bad Request";
            out_response->json_body = "{\"ok\":false,\"error\":\"invalid_request\"}";
            return true;
        case ARGUS_BROWSER_ENDPOINT_FORBIDDEN:
            out_response->status_code = 403;
            out_response->status_line = "403 Forbidden";
            out_response->json_body = "{\"ok\":false,\"error\":\"command_not_admitted\"}";
            return true;
        case ARGUS_BROWSER_ENDPOINT_CONFLICT:
            out_response->status_code = 409;
            out_response->status_line = "409 Conflict";
            out_response->json_body = "{\"ok\":false,\"error\":\"command_conflict\"}";
            return true;
        case ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR:
            out_response->status_code = 500;
            out_response->status_line = "500 Internal Server Error";
            out_response->json_body = "{\"ok\":false,\"error\":\"internal_error\"}";
            return true;
        default:
            memset(out_response, 0, sizeof(*out_response));
            return false;
    }
}
