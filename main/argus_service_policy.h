#pragma once

#include "argus_net_mgr.h"
#include "argus_authority_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

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

argus_svc_policy_result_t argus_service_policy_evaluate_entry_for_owner(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *auth,
    argus_authority_owner_t requested_owner,
    argus_net_event_t *out_evt
);

bool argus_service_entry_fingerprint_matches(
    const argus_service_entry_fingerprint_t *expected,
    const argus_service_entry_fingerprint_t *actual
);

bool argus_service_policy_entry_permitted(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *auth
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
