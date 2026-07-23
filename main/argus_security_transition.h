#ifndef ARGUS_SECURITY_TRANSITION_H
#define ARGUS_SECURITY_TRANSITION_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*argus_security_transition_step_fn)(void *ctx);

typedef struct {
    bool resource_ready;
    bool reserved;
    bool armed;
    bool callback_claimed;
} argus_security_transition_t;

void argus_security_transition_set_resource_ready(
    argus_security_transition_t *transition,
    bool ready);
esp_err_t argus_security_transition_prepare(
    argus_security_transition_t *transition);
void argus_security_transition_cancel(
    argus_security_transition_t *transition);
esp_err_t argus_security_transition_respond_then_arm(
    argus_security_transition_t *transition,
    argus_security_transition_step_fn respond,
    argus_security_transition_step_fn arm,
    void *ctx);
bool argus_security_transition_claim_callback(
    argus_security_transition_t *transition);

#ifdef __cplusplus
}
#endif

#endif
