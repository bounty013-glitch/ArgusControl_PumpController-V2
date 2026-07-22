/**
 * @file argus_browser_command_endpoint.h
 * @brief Phase 4B.4 authenticated browser-command admission and dispatch core.
 */

#ifndef ARGUS_BROWSER_COMMAND_ENDPOINT_H
#define ARGUS_BROWSER_COMMAND_ENDPOINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "argus_authority_mgr.h"
#include "argus_browser_command.h"
#include "argus_net_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_BROWSER_COMMAND_URI "/api/command"
#define ARGUS_BROWSER_BODY_RECV_TIMEOUT (-1000)

typedef enum {
    ARGUS_BROWSER_BODY_RECEIVE_OK = 0,
    ARGUS_BROWSER_BODY_RECEIVE_INVALID_ARGUMENT,
    ARGUS_BROWSER_BODY_RECEIVE_EMPTY,
    ARGUS_BROWSER_BODY_RECEIVE_TOO_LARGE,
    ARGUS_BROWSER_BODY_RECEIVE_TRUNCATED,
    ARGUS_BROWSER_BODY_RECEIVE_TIMEOUT,
    ARGUS_BROWSER_BODY_RECEIVE_ERROR,
} argus_browser_body_receive_result_t;

typedef int (*argus_browser_body_recv_fn_t)(void *ctx,
                                            uint8_t *dst,
                                            size_t max_len);

/**
 * @brief Receive one complete length-delimited command body without relying on
 *        a trailing NUL byte.
 */
argus_browser_body_receive_result_t argus_browser_command_receive_body(
    size_t content_len,
    argus_browser_body_recv_fn_t recv_fn,
    void *recv_ctx,
    uint8_t *out_body,
    size_t out_capacity,
    size_t *out_len);

typedef struct {
    esp_err_t (*get_network_snapshot)(void *ctx, argus_net_snapshot_t *out_snap);
    esp_err_t (*get_authority_snapshot)(void *ctx, argus_authority_snapshot_t *out_snap);
    esp_err_t (*dispatch)(void *ctx, const argus_command_envelope_t *envelope);
    void *ctx;
} argus_browser_command_endpoint_ops_t;

typedef enum {
    ARGUS_BROWSER_ENDPOINT_OK = 0,
    ARGUS_BROWSER_ENDPOINT_UNAUTHORIZED,
    ARGUS_BROWSER_ENDPOINT_BAD_REQUEST,
    ARGUS_BROWSER_ENDPOINT_FORBIDDEN,
    ARGUS_BROWSER_ENDPOINT_CONFLICT,
    ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR,
} argus_browser_command_endpoint_result_t;

typedef struct {
    argus_browser_command_endpoint_result_t result;
    argus_browser_command_decode_result_t decode_result;
    esp_err_t operation_error;
    bool envelope_built;
    argus_command_envelope_t envelope;
} argus_browser_command_endpoint_outcome_t;

/**
 * @brief Authenticate, decode, admit, construct, and dispatch one browser
 *        command through injected production or test operations.
 */
argus_browser_command_endpoint_result_t argus_browser_command_endpoint_process(
    bool authenticated,
    const uint8_t *body,
    size_t body_len,
    const argus_browser_command_endpoint_ops_t *ops,
    argus_browser_command_endpoint_outcome_t *out_outcome);

typedef struct {
    int status_code;
    const char *status_line;
    const char *json_body;
    const char *cache_control;
} argus_browser_command_http_response_t;

/**
 * @brief Return the bounded HTTP response contract for an endpoint result.
 */
bool argus_browser_command_response_for(
    argus_browser_command_endpoint_result_t result,
    argus_browser_command_http_response_t *out_response);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_BROWSER_COMMAND_ENDPOINT_H */
