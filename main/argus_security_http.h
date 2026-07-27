#ifndef ARGUS_SECURITY_HTTP_H
#define ARGUS_SECURITY_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "argus_authorization.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t argus_security_http_init(void);
esp_err_t argus_security_http_register(httpd_handle_t server);

typedef struct {
    bool mutation_committed;
    bool disconnect_machine;
    bool quarantine_rotation;
    bool disclose_rotation_secret;
    esp_err_t response_error;
} argus_security_http_machine_action_decision_t;

/* Body-shape verdict for POST /api/security/machines/action.
 *
 * Exposed so the suite can drive the REAL decision - exact key set, field
 * bounds, and permission-array validation - rather than a transcription of
 * it. set_permissions was unreachable for exactly the want of this: it needs
 * a third field, the key check accepted only two, and no test ever submitted
 * a complete body through this path. */
typedef enum {
    ARGUS_MACHINE_ACTION_BODY_OK = 0,
    ARGUS_MACHINE_ACTION_BODY_INVALID,              /* shape, keys, id, action */
    ARGUS_MACHINE_ACTION_BODY_INVALID_PERMISSIONS,  /* permissions unusable */
} argus_security_http_machine_body_t;

struct cJSON;

argus_security_http_machine_body_t argus_security_http_machine_action_body(
    const struct cJSON *root,
    const char **out_identifier, const char **out_action,
    bool *out_set_permissions, argus_permission_set_t *out_permissions);

void argus_security_http_machine_action_decide(
    esp_err_t mutation_error, esp_err_t audit_finalization_error,
    bool rotation,
    argus_security_http_machine_action_decision_t *out);

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
typedef struct {
    uint64_t before_sequence;
    uint32_t limit;
} argus_security_http_audit_query_t;

size_t argus_security_http_test_route_count(void);
bool argus_security_http_test_registered_route(
    size_t index, const char **path, httpd_method_t *method);
bool argus_security_http_test_parse_audit_query(
    const char *query,
    argus_security_http_audit_query_t *out);
#endif

#ifdef __cplusplus
}
#endif

#endif
