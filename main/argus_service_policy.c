#include "argus_service_policy.h"
#include <string.h>

static bool recovery_state_is_eligible(const argus_net_snapshot_t *net)
{
    if (net->wifi_transaction_active) {
        return net->apply_state == ARGUS_WIFI_APPLY_PREPARING ||
               net->apply_state == ARGUS_WIFI_APPLY_WAITING_DISCONNECT ||
               net->apply_state == ARGUS_WIFI_APPLY_APPLYING_CONFIG ||
               net->apply_state == ARGUS_WIFI_APPLY_CONNECTING;
    }

    return net->sta_state == ARGUS_STA_RETRY_WAIT ||
           net->sta_state == ARGUS_STA_ACTION_REQUIRED ||
           net->sta_state == ARGUS_STA_IDLE;
}

static void capture_entry_fingerprint(argus_service_entry_fingerprint_t *out,
                                      const argus_net_snapshot_t *net,
                                      const argus_authority_snapshot_t *auth)
{
    memset(out, 0, sizeof(*out));
    out->mode = net->mode;
    out->sta_state = net->sta_state;
    out->apply_state = net->apply_state;
    out->authority_mode = auth->mode;
    out->authority_owner = auth->owner;
    out->authority_generation = auth->generation;
    out->timer_generation = net->timer_generation;
    out->transaction_generation = net->transaction_generation;
    out->auto_retry_timer_generation = net->auto_retry_timer_generation;
    out->ip_timeout_timer_generation = net->ip_timeout_timer_generation;
    out->consecutive_failures = net->consecutive_failures;
    out->last_disconnect_reason = net->last_disconnect_reason;
    out->last_disconnect_category = net->last_disconnect_category;
    out->sta_connected = net->sta_connected;
    out->sta_ip_acquired = net->sta_ip_acquired;
    out->ap_started = net->ap_started;
    out->mqtt_broker_running = net->mqtt_broker_running;
    out->mqtt_broker_stopped = net->mqtt_broker_stopped;
    out->mqtt_broker_observable = net->mqtt_broker_observable;
    out->commissioned = net->commissioned;
    out->wifi_transaction_active = net->wifi_transaction_active;
    out->auto_retry_timer_active = net->auto_retry_timer_active;
    out->ip_timeout_timer_active = net->ip_timeout_timer_active;
}

bool argus_service_entry_fingerprint_matches(
    const argus_service_entry_fingerprint_t *expected,
    const argus_service_entry_fingerprint_t *actual)
{
    if (!expected || !actual) return false;
    return expected->mode == actual->mode &&
           expected->sta_state == actual->sta_state &&
           expected->apply_state == actual->apply_state &&
           expected->authority_mode == actual->authority_mode &&
           expected->authority_owner == actual->authority_owner &&
           expected->authority_generation == actual->authority_generation &&
           expected->timer_generation == actual->timer_generation &&
           expected->transaction_generation == actual->transaction_generation &&
           expected->auto_retry_timer_generation == actual->auto_retry_timer_generation &&
           expected->ip_timeout_timer_generation == actual->ip_timeout_timer_generation &&
           expected->consecutive_failures == actual->consecutive_failures &&
           expected->last_disconnect_reason == actual->last_disconnect_reason &&
           expected->last_disconnect_category == actual->last_disconnect_category &&
           expected->sta_connected == actual->sta_connected &&
           expected->sta_ip_acquired == actual->sta_ip_acquired &&
           expected->ap_started == actual->ap_started &&
           expected->mqtt_broker_running == actual->mqtt_broker_running &&
           expected->mqtt_broker_stopped == actual->mqtt_broker_stopped &&
           expected->mqtt_broker_observable == actual->mqtt_broker_observable &&
           expected->commissioned == actual->commissioned &&
           expected->wifi_transaction_active == actual->wifi_transaction_active &&
           expected->auto_retry_timer_active == actual->auto_retry_timer_active &&
           expected->ip_timeout_timer_active == actual->ip_timeout_timer_active;
}

argus_svc_policy_result_t argus_service_policy_evaluate_entry_for_owner(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *auth,
    argus_authority_owner_t requested_owner,
    argus_net_event_t *out_evt)
{
    if (!net || !auth || !out_evt) return ARGUS_SVC_POLICY_REJECT_MODE;
    if (requested_owner != ARGUS_AUTH_OWNER_BROWSER &&
        requested_owner != ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
        return ARGUS_SVC_POLICY_REJECT_AUTHORITY;
    }

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

    if (!net->ap_started) {
        return ARGUS_SVC_POLICY_REJECT_MODE;
    }

    /* Mode gate and complete authority/network combinations. */
    if (net->mode == ARGUS_NET_MODE_AP_DISCOVERABLE) {
        bool normal = auth->mode == ARGUS_AUTHORITY_SUPERVISORY &&
                      auth->owner == ARGUS_AUTH_OWNER_MQTT;
        bool recovery = auth->mode == ARGUS_AUTHORITY_NONE &&
                        auth->owner == ARGUS_AUTH_OWNER_NONE &&
                        net->commissioned &&
                        !net->sta_connected &&
                        !net->sta_ip_acquired &&
                        net->mqtt_broker_observable &&
                        net->mqtt_broker_stopped &&
                        !net->mqtt_broker_running &&
                        recovery_state_is_eligible(net);
        if (!normal && !recovery) {
            return ARGUS_SVC_POLICY_REJECT_AUTHORITY;
        }
    } else if (net->mode == ARGUS_NET_MODE_UNCOMMISSIONED_AP) {
        if (auth->mode != ARGUS_AUTHORITY_NONE || auth->owner != ARGUS_AUTH_OWNER_NONE) {
            return ARGUS_SVC_POLICY_REJECT_AUTHORITY;
        }
    } else {
        return ARGUS_SVC_POLICY_REJECT_MODE;
    }

    memset(out_evt, 0, sizeof(argus_net_event_t));
    out_evt->type = ARGUS_NET_EVT_SERVICE_REQUEST;
    out_evt->requested_owner = requested_owner;
    out_evt->service_preflight_required = true;
    capture_entry_fingerprint(&out_evt->service_preflight, net, auth);

    return ARGUS_SVC_POLICY_OK;
}

argus_svc_policy_result_t argus_service_policy_evaluate_entry(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *auth,
    argus_net_event_t *out_evt)
{
    return argus_service_policy_evaluate_entry_for_owner(
        net, auth, ARGUS_AUTH_OWNER_BROWSER, out_evt);
}

bool argus_service_policy_entry_permitted(
    const argus_net_snapshot_t *net,
    const argus_authority_snapshot_t *auth)
{
    argus_net_event_t evt = {0};
    return argus_service_policy_evaluate_entry(net, auth, &evt) ==
           ARGUS_SVC_POLICY_OK;
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
