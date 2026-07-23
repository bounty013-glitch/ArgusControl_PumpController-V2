#ifndef ARGUS_SECURITY_HTTP_H
#define ARGUS_SECURITY_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t argus_security_http_init(void);
esp_err_t argus_security_http_register(httpd_handle_t server);

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
