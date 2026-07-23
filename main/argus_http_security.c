#include "argus_http_security.h"

#include <stdio.h>
#include <string.h>

#include "argus_security_directory.h"
#include "argus_security_audit.h"
#include "argus_security_store.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#define COOKIE_HEADER_MAX 256U
#define HOST_HEADER_MAX 64U
#define ORIGIN_HEADER_MAX 80U

static uint64_t s_last_sta_rejection_audit_us;

static bool socket_address_ipv4(
    const struct sockaddr_storage *address, uint32_t *out)
{
    if (address == NULL || out == NULL) return false;
    if (address->ss_family == AF_INET) {
        const struct sockaddr_in *ipv4 =
            (const struct sockaddr_in *)address;
        *out = ipv4->sin_addr.s_addr;
        return true;
    }
    if (address->ss_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 =
            (const struct sockaddr_in6 *)address;
        if (!IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr)) return false;
        memcpy(out, &ipv6->sin6_addr.s6_addr[12], sizeof(*out));
        return true;
    }
    return false;
}

static bool socket_is_softap(
    httpd_req_t *req, uint32_t *out_peer_key,
    char *out_local_ip, size_t out_local_ip_size)
{
    if (req == NULL || out_peer_key == NULL ||
        out_local_ip == NULL || out_local_ip_size < 16U) {
        return false;
    }
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage local = {0};
    struct sockaddr_storage peer = {0};
    socklen_t local_len = sizeof(local);
    socklen_t peer_len = sizeof(peer);
    if (fd < 0 ||
        getsockname(fd, (struct sockaddr *)&local, &local_len) != 0 ||
        getpeername(fd, (struct sockaddr *)&peer, &peer_len) != 0) {
        return false;
    }
    uint32_t local_addr;
    uint32_t peer_addr;
    if (!socket_address_ipv4(&local, &local_addr) ||
        !socket_address_ipv4(&peer, &peer_addr)) return false;

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip = {0};
    if (ap == NULL || esp_netif_get_ip_info(ap, &ip) != ESP_OK ||
        ip.ip.addr == 0U || local_addr != ip.ip.addr) {
        return false;
    }
    if ((peer_addr & ip.netmask.addr) !=
        (ip.ip.addr & ip.netmask.addr) ||
        peer_addr == ip.ip.addr || peer_addr == 0U) {
        return false;
    }
    struct in_addr printable = {.s_addr = local_addr};
    if (inet_ntop(AF_INET, &printable,
                  out_local_ip, out_local_ip_size) == NULL) {
        return false;
    }
    *out_peer_key = peer_addr;
    return true;
}

void argus_http_security_headers(httpd_req_t *req, bool html)
{
    if (req == NULL) return;
    httpd_resp_set_type(req, html ? "text/html; charset=utf-8" :
                                    "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(
        req, "Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'unsafe-inline'; "
        "style-src 'self' 'unsafe-inline'; connect-src 'self'; "
        "img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; "
        "form-action 'self'");
}

argus_http_access_result_t argus_http_security_ap_only(
    httpd_req_t *req, uint32_t *out_peer_key)
{
    char local_ip[16];
    uint32_t peer_key;
    if (!socket_is_softap(req, &peer_key, local_ip, sizeof(local_ip))) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (s_last_sta_rejection_audit_us == 0U ||
            now_us - s_last_sta_rejection_audit_us >=
                UINT64_C(60000000)) {
            argus_security_store_status_t status = {0};
            (void)argus_security_store_get_status(&status);
            (void)argus_security_audit_append(
                ARGUS_AUDIT_STA_ROUTE_REJECTED,
                ARGUS_AUDIT_OUTCOME_REJECTED,
                ARGUS_PRINCIPAL_NONE, "anonymous", "human_route",
                "NON_SOFTAP", "local_ap_required",
                status.security_epoch, false);
            s_last_sta_rejection_audit_us = now_us;
        }
        return ARGUS_HTTP_ACCESS_NOT_AP;
    }
    if (out_peer_key != NULL) *out_peer_key = peer_key;
    return ARGUS_HTTP_ACCESS_OK;
}

static bool header_exact(httpd_req_t *req, const char *name,
                         char *out, size_t capacity)
{
    size_t length = httpd_req_get_hdr_value_len(req, name);
    return length > 0U && length < capacity &&
           httpd_req_get_hdr_value_str(req, name, out, capacity) == ESP_OK &&
           strlen(out) == length;
}

bool argus_http_security_json_content_type(httpd_req_t *req)
{
    char value[48];
    return header_exact(req, "Content-Type", value, sizeof(value)) &&
           strcmp(value, "application/json") == 0;
}

static bool expected_origin(httpd_req_t *req, char *out, size_t capacity)
{
    uint32_t peer;
    char local_ip[16];
    if (!socket_is_softap(req, &peer, local_ip, sizeof(local_ip))) {
        return false;
    }
    (void)peer;
    int written = snprintf(out, capacity, "http://%s", local_ip);
    return written > 0 && (size_t)written < capacity;
}

argus_http_access_result_t argus_http_security_origin(
    httpd_req_t *req)
{
    char expected[32];
    char origin[ORIGIN_HEADER_MAX];
    char host[HOST_HEADER_MAX];
    if (!expected_origin(req, expected, sizeof(expected)) ||
        !header_exact(req, "Origin", origin, sizeof(origin)) ||
        !header_exact(req, "Host", host, sizeof(host)) ||
        strcmp(origin, expected) != 0) {
        return ARGUS_HTTP_ACCESS_ORIGIN;
    }
    const char *expected_host = expected + strlen("http://");
    if (strcmp(host, expected_host) != 0) {
        char with_port[32];
        int written = snprintf(with_port, sizeof(with_port),
                               "%s:80", expected_host);
        if (written <= 0 || (size_t)written >= sizeof(with_port) ||
            strcmp(host, with_port) != 0) {
            return ARGUS_HTTP_ACCESS_ORIGIN;
        }
    }
    return ARGUS_HTTP_ACCESS_OK;
}

static bool parse_cookie_header(
    char *header,
    char out[ARGUS_SESSION_TOKEN_HEX_LEN + 1U])
{
    out[0] = '\0';
    bool found = false;
    char *cursor = header;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ';') cursor++;
        char *end = strchr(cursor, ';');
        if (end != NULL) *end = '\0';
        char *equals = strchr(cursor, '=');
        if (equals == NULL) return false;
        *equals = '\0';
        char *name = cursor;
        char *value = equals + 1;
        if (strcmp(name, ARGUS_SESSION_COOKIE_NAME) == 0) {
            if (found ||
                strnlen(value, ARGUS_SESSION_TOKEN_HEX_LEN + 1U) !=
                    ARGUS_SESSION_TOKEN_HEX_LEN) {
                return false;
            }
            memcpy(out, value, ARGUS_SESSION_TOKEN_HEX_LEN + 1U);
            found = true;
        }
        if (end == NULL) break;
        cursor = end + 1;
    }
    return found;
}

bool argus_http_security_cookie(
    httpd_req_t *req,
    char out[ARGUS_SESSION_TOKEN_HEX_LEN + 1U])
{
    char header[COOKIE_HEADER_MAX];
    if (out == NULL ||
        !header_exact(req, "Cookie", header, sizeof(header))) {
        return false;
    }
    bool result = parse_cookie_header(header, out);
    memset(header, 0, sizeof(header));
    if (!result) memset(out, 0, ARGUS_SESSION_TOKEN_HEX_LEN + 1U);
    return result;
}

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
bool argus_http_security_test_parse_cookie(
    const char *input,
    char out[ARGUS_SESSION_TOKEN_HEX_LEN + 1U])
{
    char header[COOKIE_HEADER_MAX];
    if (input == NULL || out == NULL ||
        strnlen(input, sizeof(header)) >= sizeof(header)) {
        return false;
    }
    strlcpy(header, input, sizeof(header));
    bool result = parse_cookie_header(header, out);
    memset(header, 0, sizeof(header));
    if (!result) memset(out, 0, ARGUS_SESSION_TOKEN_HEX_LEN + 1U);
    return result;
}

bool argus_http_security_test_normalize_address(
    bool ipv6, const uint8_t *address, size_t length, uint32_t *out)
{
    if (address == NULL || out == NULL ||
        length != (ipv6 ? 16U : 4U)) return false;
    struct sockaddr_storage storage = {0};
    if (ipv6) {
        struct sockaddr_in6 *value = (struct sockaddr_in6 *)&storage;
        value->sin6_family = AF_INET6;
        memcpy(value->sin6_addr.s6_addr, address, length);
    } else {
        struct sockaddr_in *value = (struct sockaddr_in *)&storage;
        value->sin_family = AF_INET;
        memcpy(&value->sin_addr.s_addr, address, length);
    }
    return socket_address_ipv4(&storage, out);
}
#endif

static bool principal_current(const argus_principal_t *principal)
{
    if (principal == NULL) return false;
    if (principal->type == ARGUS_PRINCIPAL_CONSOLE) {
        argus_security_store_status_t status;
        argus_password_verifier_t verifier;
        uint32_t version;
        esp_err_t status_err = argus_security_store_get_status(&status);
        esp_err_t verifier_err =
            argus_security_store_get_console_verifier(&verifier, &version);
        memset(&verifier, 0, sizeof(verifier));
        return status_err == ESP_OK && verifier_err == ESP_OK &&
               version == principal->credential_version &&
               status.security_epoch == principal->security_epoch &&
               status.security_epoch == principal->principal_revision;
    }
    if (principal->type == ARGUS_PRINCIPAL_HUMAN) {
        argus_security_human_record_t human;
        argus_security_store_status_t status;
        esp_err_t err = argus_security_directory_find_id(
            principal->identifier, &human, NULL);
        esp_err_t status_err =
            argus_security_store_get_status(&status);
        bool current = err == ESP_OK && status_err == ESP_OK &&
                       human.enabled != 0U &&
                       human.revoked == 0U &&
                       human.credential_version ==
                           principal->credential_version &&
                       human.record_security_epoch ==
                           principal->principal_revision &&
                       status.security_epoch == principal->security_epoch;
        memset(&human, 0, sizeof(human));
        memset(&status, 0, sizeof(status));
        return current;
    }
    return false;
}

argus_http_access_result_t argus_http_security_require(
    httpd_req_t *req,
    argus_permission_set_t required,
    bool mutation,
    argus_http_security_context_t *out)
{
    if (req == NULL || out == NULL) return ARGUS_HTTP_ACCESS_INTERNAL;
    memset(out, 0, sizeof(*out));
    argus_http_access_result_t result =
        argus_http_security_ap_only(req, &out->peer_key);
    if (result != ARGUS_HTTP_ACCESS_OK) return result;
    if (!argus_http_security_cookie(req, out->session_token) ||
        argus_session_manager_authenticate(
            out->session_token, &out->principal,
            &out->session_index) != ESP_OK ||
        !principal_current(&out->principal)) {
        if (out->session_token[0] != '\0') {
            (void)argus_session_manager_revoke_token(out->session_token);
        }
        memset(out, 0, sizeof(*out));
        return ARGUS_HTTP_ACCESS_UNAUTHENTICATED;
    }
    if (required != 0U &&
        argus_authorization_require(&out->principal, required) !=
            ARGUS_AUTHZ_ALLOW) {
        return ARGUS_HTTP_ACCESS_FORBIDDEN;
    }
    if (!mutation) return ARGUS_HTTP_ACCESS_OK;
    if (!argus_http_security_json_content_type(req)) {
        return ARGUS_HTTP_ACCESS_CONTENT_TYPE;
    }
    result = argus_http_security_origin(req);
    if (result != ARGUS_HTTP_ACCESS_OK) return result;
    char csrf[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
    if (!header_exact(req, ARGUS_CSRF_HEADER, csrf, sizeof(csrf)) ||
        !argus_session_manager_csrf_valid(out->session_index, csrf)) {
        memset(csrf, 0, sizeof(csrf));
        return ARGUS_HTTP_ACCESS_CSRF;
    }
    memset(csrf, 0, sizeof(csrf));
    return ARGUS_HTTP_ACCESS_OK;
}

esp_err_t argus_http_security_send_error(
    httpd_req_t *req, argus_http_access_result_t result)
{
    argus_http_security_headers(req, false);
    const char *status = "500 Internal Server Error";
    const char *body = "{\"ok\":false,\"error\":\"internal_error\"}";
    switch (result) {
        case ARGUS_HTTP_ACCESS_NOT_AP:
            status = "403 Forbidden";
            body = "{\"ok\":false,\"error\":\"local_ap_required\"}";
            break;
        case ARGUS_HTTP_ACCESS_UNAUTHENTICATED:
            status = "401 Unauthorized";
            body = "{\"ok\":false,\"error\":\"authentication_required\"}";
            break;
        case ARGUS_HTTP_ACCESS_FORBIDDEN:
            status = "403 Forbidden";
            body = "{\"ok\":false,\"error\":\"forbidden\"}";
            break;
        case ARGUS_HTTP_ACCESS_CSRF:
        case ARGUS_HTTP_ACCESS_ORIGIN:
            status = "403 Forbidden";
            body = "{\"ok\":false,\"error\":\"request_protection_failed\"}";
            break;
        case ARGUS_HTTP_ACCESS_CONTENT_TYPE:
            status = "415 Unsupported Media Type";
            body = "{\"ok\":false,\"error\":\"unsupported_media_type\"}";
            break;
        default:
            break;
    }
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, body);
}
