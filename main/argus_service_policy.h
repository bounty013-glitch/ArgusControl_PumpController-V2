#pragma once

#include "argus_net_mgr.h"
#include "argus_authority_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARGUS_SVC_POLICY_OK = 0,
    ARGUS_SVC_POLICY_IDEMPOTENT,
    ARGUS_SVC_POLICY_REJECT_MODE,
    ARGUS_SVC_POLICY_REJECT_AUTHORITY,
    ARGUS_SVC_POLICY_TRANSITION_IN_PROGRESS
} argus_svc_policy_result_t;

/**
 * @brief Policy seam for HTTP /api/service/enter requests
 *
 * Caller owns snapshots and resulting event generation.
 * This ensures the HTTP handler and pure tests execute the
 * same decision rules without hitting the live network queue.
 */
argus_svc_policy_result_t argus_service_policy_evaluate_entry(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *auth,
    argus_net_event_t *out_evt
);

/**
 * @brief Policy seam for HTTP /api/service/exit requests
 */
argus_svc_policy_result_t argus_service_policy_evaluate_exit(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *auth,
    argus_net_event_t *out_evt
);

#ifdef __cplusplus
}
#endif
