/**
 * @file argus_factory_reset.h
 * @brief Phase 4B.6 configuration factory-reset request and lifecycle seam.
 */

#ifndef ARGUS_FACTORY_RESET_H
#define ARGUS_FACTORY_RESET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "argus_authority_mgr.h"
#include "argus_net_mgr.h"
#include "argus_state_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_FACTORY_RESET_URI "/api/factory-reset"
#define ARGUS_FACTORY_RESET_MAX_BODY_LEN 96U
#define ARGUS_FACTORY_RESET_CONFIRMATION "FACTORY_RESET"
#define ARGUS_FACTORY_RESET_BODY_RECV_TIMEOUT (-1000)

typedef enum {
    ARGUS_FACTORY_RESET_DECODE_OK = 0,
    ARGUS_FACTORY_RESET_DECODE_INVALID_ARGUMENT,
    ARGUS_FACTORY_RESET_DECODE_EMPTY,
    ARGUS_FACTORY_RESET_DECODE_TOO_LARGE,
    ARGUS_FACTORY_RESET_DECODE_MALFORMED,
    ARGUS_FACTORY_RESET_DECODE_MISSING_CONFIRMATION,
    ARGUS_FACTORY_RESET_DECODE_DUPLICATE_FIELD,
    ARGUS_FACTORY_RESET_DECODE_UNKNOWN_FIELD,
    ARGUS_FACTORY_RESET_DECODE_WRONG_CONFIRMATION,
} argus_factory_reset_decode_result_t;

typedef enum {
    ARGUS_FACTORY_RESET_RECEIVE_OK = 0,
    ARGUS_FACTORY_RESET_RECEIVE_INVALID_ARGUMENT,
    ARGUS_FACTORY_RESET_RECEIVE_EMPTY,
    ARGUS_FACTORY_RESET_RECEIVE_TOO_LARGE,
    ARGUS_FACTORY_RESET_RECEIVE_TRUNCATED,
    ARGUS_FACTORY_RESET_RECEIVE_TIMEOUT,
    ARGUS_FACTORY_RESET_RECEIVE_ERROR,
} argus_factory_reset_receive_result_t;

typedef int (*argus_factory_reset_recv_fn_t)(void *ctx, uint8_t *dst,
                                              size_t max_len);

typedef struct {
    bool continue_request;
    bool close_session;
    int status_code;
    esp_err_t handler_result_after_response;
} argus_factory_reset_receive_disposition_t;

argus_factory_reset_decode_result_t argus_factory_reset_decode(
    const uint8_t *body, size_t body_len);
bool argus_factory_reset_content_type_valid(const char *content_type);
argus_factory_reset_receive_result_t argus_factory_reset_receive_body(
    size_t content_len, argus_factory_reset_recv_fn_t recv_fn, void *recv_ctx,
    uint8_t *out_body, size_t out_capacity, size_t *out_len);
bool argus_factory_reset_receive_disposition(
    argus_factory_reset_receive_result_t result,
    argus_factory_reset_receive_disposition_t *out);

typedef enum {
    ARGUS_FACTORY_RESET_POLICY_OK = 0,
    ARGUS_FACTORY_RESET_POLICY_WRONG_NETWORK,
    ARGUS_FACTORY_RESET_POLICY_WRONG_AUTHORITY,
    ARGUS_FACTORY_RESET_POLICY_LIFECYCLE_CONFLICT,
    ARGUS_FACTORY_RESET_POLICY_UNSAFE_MACHINE,
    ARGUS_FACTORY_RESET_POLICY_NETWORK_NOT_CONVERGED,
} argus_factory_reset_policy_result_t;

argus_factory_reset_policy_result_t argus_factory_reset_evaluate_policy(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *authority,
    const argus_state_snapshot_t *state,
    bool lifecycle_conflict);

typedef enum {
    ARGUS_FACTORY_RESET_STEP_LOCK_LIFECYCLE = 1,
    ARGUS_FACTORY_RESET_STEP_LOCK_DISPATCH,
    ARGUS_FACTORY_RESET_STEP_PREFLIGHT,
    ARGUS_FACTORY_RESET_STEP_BEGIN_TRANSITION,
    ARGUS_FACTORY_RESET_STEP_RESPONSE_GRACE,
    ARGUS_FACTORY_RESET_STEP_STOP_HTTP,
    ARGUS_FACTORY_RESET_STEP_FINAL_REVALIDATION,
    ARGUS_FACTORY_RESET_STEP_REVOKE_AUTHORITY,
    ARGUS_FACTORY_RESET_STEP_ERASE_CONFIG,
    ARGUS_FACTORY_RESET_STEP_REBOOT,
} argus_factory_reset_step_t;

typedef struct {
    esp_err_t (*lock_lifecycle)(void *ctx);
    void (*unlock_lifecycle)(void *ctx);
    void (*lock_dispatch)(void *ctx);
    void (*unlock_dispatch)(void *ctx);
    esp_err_t (*get_network_snapshot)(void *ctx, argus_net_snapshot_t *out);
    esp_err_t (*get_authority_snapshot)(void *ctx,
                                        argus_authority_snapshot_t *out);
    void (*get_state_snapshot)(void *ctx, argus_state_snapshot_t *out);
    esp_err_t (*begin_transition)(void *ctx);
    void (*response_grace_delay)(void *ctx);
    esp_err_t (*stop_http)(void *ctx);
    esp_err_t (*revoke_authority)(void *ctx);
    esp_err_t (*erase_configuration)(void *ctx);
    void (*reboot)(void *ctx);
    void *ctx;
} argus_factory_reset_ops_t;

typedef struct {
    bool accepted;
    int failed_at_step;
    bool authority_revoked;
    bool http_stopped;
    bool configuration_erased;
    bool reboot_called;
    esp_err_t error;
} argus_factory_reset_result_t;

argus_factory_reset_result_t argus_factory_reset_execute(
    const argus_factory_reset_ops_t *ops);

/** Populate operations owned by the production network lifecycle manager. */
void argus_factory_reset_get_production_ops(argus_factory_reset_ops_t *out_ops);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_FACTORY_RESET_H */
