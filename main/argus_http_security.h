#ifndef ARGUS_HTTP_SECURITY_H
#define ARGUS_HTTP_SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_authorization.h"
#include "argus_session_manager.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_SESSION_COOKIE_NAME "argus_session"
#define ARGUS_CSRF_HEADER "X-Argus-CSRF"

typedef struct {
    argus_principal_t principal;
    size_t session_index;
    uint32_t peer_key;
    char session_token[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
} argus_http_security_context_t;

typedef enum {
    ARGUS_HTTP_ACCESS_OK = 0,
    ARGUS_HTTP_ACCESS_NOT_AP,
    ARGUS_HTTP_ACCESS_UNAUTHENTICATED,
    ARGUS_HTTP_ACCESS_FORBIDDEN,
    ARGUS_HTTP_ACCESS_CSRF,
    ARGUS_HTTP_ACCESS_ORIGIN,
    ARGUS_HTTP_ACCESS_CONTENT_TYPE,
    ARGUS_HTTP_ACCESS_INTERNAL,
} argus_http_access_result_t;

void argus_http_security_headers(httpd_req_t *req, bool html);
argus_http_access_result_t argus_http_security_ap_only(
    httpd_req_t *req, uint32_t *out_peer_key);
argus_http_access_result_t argus_http_security_origin(
    httpd_req_t *req);
argus_http_access_result_t argus_http_security_require(
    httpd_req_t *req,
    argus_permission_set_t required,
    bool mutation,
    argus_http_security_context_t *out);
esp_err_t argus_http_security_send_error(
    httpd_req_t *req, argus_http_access_result_t result);
bool argus_http_security_cookie(
    httpd_req_t *req,
    char out[ARGUS_SESSION_TOKEN_HEX_LEN + 1U]);
bool argus_http_security_json_content_type(httpd_req_t *req);

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
bool argus_http_security_test_parse_cookie(
    const char *input,
    char out[ARGUS_SESSION_TOKEN_HEX_LEN + 1U]);
bool argus_http_security_test_normalize_address(
    bool ipv6, const uint8_t *address, size_t length, uint32_t *out);
#endif

#ifdef __cplusplus
}
#endif

#endif
