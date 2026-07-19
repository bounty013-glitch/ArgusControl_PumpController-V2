#include "argus_service_policy.h"
#include <string.h>

argus_svc_policy_result_t argus_service_policy_evaluate_entry(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *auth,
    argus_net_event_t *out_evt)
{
    if (!net || !auth || !out_evt) return ARGUS_SVC_POLICY_REJECT_MODE;

    /* Idempotent: already in SERVICE_AP_ONLY with BROWSER authority */
    if (net->mode == ARGUS_NET_MODE_SERVICE_AP_ONLY) {
        if (auth->mode == ARGUS_AUTHORITY_LOCAL_SERVICE &&
            auth->owner == ARGUS_AUTH_OWNER_BROWSER) {
            return ARGUS_SVC_POLICY_IDEMPOTENT;
        }
    }

    /* Transition in progress check */
    if (net->mode == ARGUS_NET_MODE_SERVICE_TRANSITION) {
        return ARGUS_SVC_POLICY_TRANSITION_IN_PROGRESS;
    }

    /* Mode gate: must be AP_DISCOVERABLE or UNCOMMISSIONED_AP */
    if (net->mode != ARGUS_NET_MODE_AP_DISCOVERABLE &&
        net->mode != ARGUS_NET_MODE_UNCOMMISSIONED_AP) {
        return ARGUS_SVC_POLICY_REJECT_MODE;
    }

    memset(out_evt, 0, sizeof(argus_net_event_t));
    out_evt->type = ARGUS_NET_EVT_SERVICE_REQUEST;
    out_evt->requested_owner = ARGUS_AUTH_OWNER_BROWSER;

    return ARGUS_SVC_POLICY_OK;
}

argus_svc_policy_result_t argus_service_policy_evaluate_exit(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *auth,
    argus_net_event_t *out_evt)
{
    if (!net || !auth || !out_evt) return ARGUS_SVC_POLICY_REJECT_MODE;

    /* Mode gate: must be SERVICE_AP_ONLY */
    if (net->mode != ARGUS_NET_MODE_SERVICE_AP_ONLY) {
        return ARGUS_SVC_POLICY_REJECT_MODE;
    }

    /* Authority gate: must be LOCAL_SERVICE/BROWSER */
    if (auth->mode != ARGUS_AUTHORITY_LOCAL_SERVICE ||
        auth->owner != ARGUS_AUTH_OWNER_BROWSER) {
        return ARGUS_SVC_POLICY_REJECT_AUTHORITY;
    }

    memset(out_evt, 0, sizeof(argus_net_event_t));
    out_evt->type = ARGUS_NET_EVT_SERVICE_EXIT;
    out_evt->requested_owner = ARGUS_AUTH_OWNER_BROWSER;

    return ARGUS_SVC_POLICY_OK;
}
