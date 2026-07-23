#include "argus_security_transition.h"

#include <string.h>

void argus_security_transition_set_resource_ready(
    argus_security_transition_t *transition,
    bool ready)
{
    if (transition == NULL) return;
    if (!ready) {
        memset(transition, 0, sizeof(*transition));
        return;
    }
    transition->resource_ready = true;
}

esp_err_t argus_security_transition_prepare(
    argus_security_transition_t *transition)
{
    if (transition == NULL || !transition->resource_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (transition->reserved || transition->armed) {
        return ESP_ERR_NOT_FINISHED;
    }
    transition->reserved = true;
    transition->callback_claimed = false;
    return ESP_OK;
}

void argus_security_transition_cancel(
    argus_security_transition_t *transition)
{
    if (transition == NULL || transition->armed) return;
    transition->reserved = false;
    transition->callback_claimed = false;
}

esp_err_t argus_security_transition_respond_then_arm(
    argus_security_transition_t *transition,
    argus_security_transition_step_fn respond,
    argus_security_transition_step_fn arm,
    void *ctx)
{
    if (transition == NULL || respond == NULL || arm == NULL ||
        !transition->resource_ready || !transition->reserved ||
        transition->armed) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = respond(ctx);
    if (err != ESP_OK) {
        argus_security_transition_cancel(transition);
        return err;
    }
    err = arm(ctx);
    if (err != ESP_OK) {
        argus_security_transition_cancel(transition);
        return err;
    }
    transition->reserved = false;
    transition->armed = true;
    return ESP_OK;
}

bool argus_security_transition_claim_callback(
    argus_security_transition_t *transition)
{
    if (transition == NULL || !transition->armed ||
        transition->callback_claimed) {
        return false;
    }
    transition->callback_claimed = true;
    transition->armed = false;
    return true;
}
