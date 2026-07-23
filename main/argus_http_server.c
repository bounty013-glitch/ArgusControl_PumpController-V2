/**
 * @file argus_http_server.c
 * @brief Controller-Hosted HTTP Server — Phase 4B.1/4B.2/4B.3 Service Portal
 *
 * Architectural decisions:
 *
 * 1. MAX CONNECTIONS = 4
 *    A mobile browser may open 2-3 parallel TCP connections (page + XHR).
 *    One associated AP station does not mean one TCP socket. Four connections
 *    provides headroom for one browser client without over-committing DRAM.
 *
 * 2. LIFECYCLE SERIALIZATION
 *    A static mutex (s_lifecycle_mutex) serializes all start/stop transitions.
 *    The server handle (s_server) is read/written only under this mutex.
 *    This prevents double-start, stale-handle, and concurrent-stop races.
 *    A failed httpd_stop() preserves s_server to prevent unverified duplicate
 *    starts — the lifecycle state remains RUNNING/UNKNOWN, not falsely STOPPED.
 *
 * 3. SELF-STOP DEADLOCK AVOIDANCE
 *    An HTTP handler must never call argus_http_server_stop() synchronously.
 *    esp_http_server dispatches handlers on its own task; stopping the server
 *    from within a handler would deadlock or destroy the calling context.
 *    Future phases (4B.3+) that need service transitions will post events
 *    to the network manager task for deferred lifecycle changes.
 *
 * 4. HANDLER / NETWORK-MANAGER LOCK GRAPH
 *    HTTP handlers use argus_net_mgr_get_snapshot() which takes s_net_mutex.
 *    This is safe because all HTTP server lifecycle calls (start/stop) in
 *    argus_net_mgr.c occur OUTSIDE s_net_mutex. The network manager releases
 *    s_net_mutex before calling httpd_stop(), so an active handler holding
 *    s_net_mutex cannot deadlock with the stop.
 *
 * 5. NO SECRETS
 *    No endpoint returns passwords, AP credentials, or sensitive NVS data.
 *    GET /api/status and GET /api/identity return only non-secret fields.
 *
 * 6. SECURITY BASELINE
 *    - No permissive CORS (no Access-Control-Allow-Origin header).
 *    - Cache-Control: no-store on all API responses.
 *    - Registered GET endpoints are read-only; state-changing operations use
 *      explicitly registered, authenticated POST handlers with method and
 *      content-type enforcement. Unregistered methods are rejected.
 *    - Content-Type: application/json on all API responses.
 *    - No Internet/CDN dependency — all assets embedded.
 *
 * 7. SERVICE AP INTERFACE ENFORCEMENT
 *    Human browser routes are admitted only through the controller SoftAP.
 *    The Phase 4D.3 middleware validates the accepted socket, Host, Origin,
 *    session, capability, and CSRF contract before protected work begins.
 *
 * 8. CREDENTIAL PROTECTION
 *    Browser access uses the encrypted Phase 4D credential directory and
 *    bounded RAM-only sessions. Legacy HTTP Basic Auth and plaintext portal
 *    password storage are not accepted compatibility paths.
 */

#include "argus_http_server.h"
#include "argus_identity.h"
#include "argus_state_mgr.h"
#include "argus_authority_mgr.h"
#include "argus_net_mgr.h"
#include "argus_mqtt_broker.h"
#include "argus_nvs_config.h"
#include "argus_config_overlay.h"
#include "argus_json.h"
#include "argus_service_policy.h"
#include "argus_browser_command_endpoint.h"
#include "argus_cmd_router.h"
#include "argus_factory_reset.h"
#include "argus_password_verifier.h"
#include "argus_security_store.h"
#include "argus_auth_service.h"
#include "argus_http_security.h"
#include "argus_session_manager.h"
#include "argus_authorization.h"
#include "argus_security_http.h"
#include "argus_security_admin.h"
#include "argus_security_audit.h"
#include "argus_security_directory.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "argus_http";
static uint64_t s_last_login_failure_audit_us;

/* ── Lifecycle state ─────────────────────────────────────────────── */

static SemaphoreHandle_t s_lifecycle_mutex = NULL;
static httpd_handle_t    s_server = NULL;
static bool              s_initialized = false;

#define HTTP_MAX_CONNECTIONS  4
#define HTTP_RECV_TIMEOUT_S   5
#define HTTP_SEND_TIMEOUT_S   5
#define HTTP_STACK_SIZE       6144


static bool require_access(
    httpd_req_t *req, argus_permission_set_t capability, bool mutation,
    argus_http_security_context_t *out)
{
    argus_http_security_context_t local;
    argus_http_access_result_t result = argus_http_security_require(
        req, capability, mutation, out != NULL ? out : &local);
    if (result == ARGUS_HTTP_ACCESS_OK) return true;
    (void)argus_http_security_send_error(req, result);
    return false;
}

static bool require_page_access(
    httpd_req_t *req,
    argus_permission_set_t capability,
    argus_http_security_context_t *out)
{
    argus_http_security_context_t local;
    argus_http_access_result_t result = argus_http_security_require(
        req, capability, false, out != NULL ? out : &local);
    if (result == ARGUS_HTTP_ACCESS_OK) return true;
    if (result == ARGUS_HTTP_ACCESS_UNAUTHENTICATED) {
        argus_http_security_headers(req, true);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        (void)httpd_resp_send(req, NULL, 0);
    } else {
        (void)argus_http_security_send_error(req, result);
    }
    return false;
}

/* ── Response helpers ────────────────────────────────────────────── */

/**
 * @brief Set standard API response headers.
 *
 * All API responses get:
 *   Content-Type: application/json
 *   Cache-Control: no-store
 *   X-Content-Type-Options: nosniff
 */
static void set_api_headers(httpd_req_t *req)
{
    argus_http_security_headers(req, false);
}

/**
 * @brief Send a method-not-allowed response for non-GET requests.
 */
static esp_err_t send_method_not_allowed(httpd_req_t *req)
{
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "GET");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Method Not Allowed");
    return ESP_OK;
}

/* ── JSON serialization helpers ──────────────────────────────────── */

/**
 * @brief Escape a string for safe JSON embedding.
 *
 * Handles: \\ \" and control characters (replaced with space).
 * Output is truncated if dst_size is exceeded.
 *
 * NOTE: JSON escaping is NOT HTML escaping. Values embedded via innerHTML
 * require a separate DOM-safe rendering path. The portal JavaScript uses
 * textContent for user-supplied values to prevent XSS.
 */
static void json_escape(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size < 2) {
        if (dst && dst_size > 0) dst[0] = '\0';
        return;
    }

    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di < dst_size - 2; si++) {
        char c = src[si];
        if (c == '"' || c == '\\') {
            if (di + 2 >= dst_size - 1) break;
            dst[di++] = '\\';
            dst[di++] = c;
        } else if ((unsigned char)c < 0x20) {
            /* Replace control characters with space */
            dst[di++] = ' ';
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

#define ARGUS_MACHINE_STATUS_JSON_MAX 512U

static int format_machine_status_json(const argus_state_snapshot_t *snapshot,
                                      char *out,
                                      size_t out_size)
{
    if (snapshot == NULL || out == NULL || out_size == 0U) {
        return -1;
    }

    char escaped_rejection[sizeof(snapshot->last_rejection_reason) * 2U + 1U];
    json_escape(snapshot->last_rejection_reason, escaped_rejection,
                sizeof(escaped_rejection));

    int len = snprintf(out, out_size,
        "{"
        "\"state\":\"%s\","
        "\"target_rpm_milli\":%" PRId32 ","
        "\"applied_rpm_milli\":%" PRId32 ","
        "\"generated_rpm_milli\":%" PRId32 ","
        "\"requested_forward\":%s,"
        "\"applied_forward\":%s,"
        "\"driver_enabled\":%s,"
        "\"estop_latched\":%s,"
        "\"ramp_active\":%s,"
        "\"fault_code\":%" PRIu32 ","
        "\"command_generation\":%" PRIu32 ","
        "\"feedback_available\":%s,"
        "\"last_rejection_reason\":\"%s\""
        "}",
        argus_state_mgr_get_state_name(snapshot->machine_state),
        snapshot->configured_target_rpm_milli,
        snapshot->applied_rpm_milli,
        snapshot->generated_rpm_milli,
        snapshot->requested_forward ? "true" : "false",
        snapshot->applied_forward ? "true" : "false",
        snapshot->driver_enabled ? "true" : "false",
        snapshot->estop_latched ? "true" : "false",
        snapshot->ramp_active ? "true" : "false",
        snapshot->fault_code,
        snapshot->command_generation,
        snapshot->feedback_available ? "true" : "false",
        escaped_rejection);

    return (len >= 0 && (size_t)len < out_size) ? len : -1;
}

/* Expose json_escape for pure testing in diagnostic builds */
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
void argus_http_test_json_escape(const char *src, char *dst, size_t dst_size)
{
    json_escape(src, dst, dst_size);
}

int argus_http_test_format_machine_status_json(
    const argus_state_snapshot_t *snapshot,
    char *out,
    size_t out_size)
{
    return format_machine_status_json(snapshot, out, out_size);
}
#endif

/* ── GET /api/status handler ─────────────────────────────────────── */

/**
 * @brief Returns non-secret system status as JSON.
 *
 * Includes: machine state, speeds (truthful labels), network mode,
 * authority mode/owner/generation, broker status.
 *
 * Does NOT include: passwords, AP credentials, NVS secrets.
 *
 * Lock safety: This handler takes s_net_mutex via argus_net_mgr_get_snapshot().
 * All HTTP server lifecycle calls (start/stop) in argus_net_mgr.c occur outside
 * s_net_mutex, so httpd_stop() waiting for this handler cannot deadlock.
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!require_access(req, ARGUS_PERMISSION_VIEW_STATUS, false, NULL)) {
        return ESP_OK;
    }

    if (req->method != HTTP_GET) {
        return send_method_not_allowed(req);
    }

    set_api_headers(req);

    /* Gather coherent snapshots from existing manager APIs */
    argus_state_snapshot_t state_snap;
    argus_state_mgr_get_snapshot(&state_snap);

    argus_net_snapshot_t net_snap;
    argus_authority_snapshot_t auth_snap;
    argus_net_event_t service_evt = {0};
    argus_svc_policy_result_t service_policy;
    if (argus_net_mgr_evaluate_service_entry(
            ARGUS_AUTH_OWNER_BROWSER, &net_snap, &auth_snap,
            &service_evt, &service_policy) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"service policy snapshot failed\"}");
        return ESP_OK;
    }

    argus_mqtt_broker_lifecycle_obs_t broker_obs = {0};
    esp_err_t broker_err = argus_mqtt_broker_get_lifecycle_obs(&broker_obs);

    const char *net_mode_str = argus_net_mgr_get_mode_name(net_snap.mode);
    const char *auth_mode_str = argus_authority_mgr_get_mode_name(auth_snap.mode);
    const char *auth_owner_str = argus_authority_mgr_get_owner_name(auth_snap.owner);

    /* NVS commissioned status — no secrets */
    bool commissioned = net_snap.commissioned;
    bool service_entry_permitted = service_policy == ARGUS_SVC_POLICY_OK;

    const char *reason_name = "NONE";
    argus_net_classify_disconnect(net_snap.last_disconnect_reason, &reason_name);
    if (net_snap.last_disconnect_reason == 0 && net_snap.last_disconnect_category == ARGUS_DISCONNECT_CAT_IP_TIMEOUT) {
        reason_name = "IP_ACQUISITION_TIMEOUT";
    }

    const char *operator_guidance = "No network issues detected.";
    const char *timer_cancel_failure = "NONE";
    if (net_snap.last_service_cancel_failure ==
        ARGUS_SERVICE_CANCEL_FAILURE_RETRY_TIMER) {
        timer_cancel_failure = "AUTO_RETRY_TIMER_STOP";
        operator_guidance = argus_net_service_cancel_guidance(
            net_snap.last_service_cancel_failure);
    } else if (net_snap.last_service_cancel_failure ==
               ARGUS_SERVICE_CANCEL_FAILURE_IP_TIMER) {
        timer_cancel_failure = "IP_TIMEOUT_TIMER_STOP";
        operator_guidance = argus_net_service_cancel_guidance(
            net_snap.last_service_cancel_failure);
    } else if (net_snap.apply_state == ARGUS_WIFI_APPLY_PREPARING ||
        net_snap.apply_state == ARGUS_WIFI_APPLY_WAITING_DISCONNECT ||
        net_snap.apply_state == ARGUS_WIFI_APPLY_APPLYING_CONFIG ||
        net_snap.apply_state == ARGUS_WIFI_APPLY_CONNECTING) {
        operator_guidance = "Wi-Fi recovery in progress; previous failure is retained until recovery succeeds.";
    } else if (net_snap.action_required) {
        operator_guidance = "Operator action required: Check Wi-Fi SSID and password.";
    } else if (net_snap.sta_state == ARGUS_STA_RETRY_WAIT) {
        operator_guidance = "Connection lost. Retrying automatically...";
    } else if (net_snap.sta_state == ARGUS_STA_DISABLED) {
        operator_guidance = "Wi-Fi client is disabled.";
    }

    const char *category_str = "NONE";
    switch (net_snap.last_disconnect_category) {
        case ARGUS_DISCONNECT_CAT_AUTHENTICATION: category_str = "AUTHENTICATION"; break;
        case ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE: category_str = "AP_UNAVAILABLE"; break;
        case ARGUS_DISCONNECT_CAT_IP_TIMEOUT: category_str = "IP_TIMEOUT"; break;
        case ARGUS_DISCONNECT_CAT_UNKNOWN: category_str = "UNKNOWN"; break;
        default: break;
    }

    char machine_json[ARGUS_MACHINE_STATUS_JSON_MAX];
    if (format_machine_status_json(&state_snap, machine_json,
                                   sizeof(machine_json)) < 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"machine status overflow\"}");
        return ESP_OK;
    }

    /* Build JSON response */
    char buf[1792];
    int len = snprintf(buf, sizeof(buf),
        "{"
        "\"machine\":%s,"
        "\"authority\":{\"mode\":\"%s\",\"owner\":\"%s\","
        "\"generation\":%" PRIu32 "},"
        "\"network\":{\"mode\":\"%s\","
        "\"sta_connected\":%s,\"sta_ip_acquired\":%s,"
        "\"ap_started\":%s,"
        "\"sta_ip_address\":\"%s\","
        "\"sta_state\":\"%s\","
        "\"recovery_state\":\"%s\","
        "\"last_error_category\":\"%s\","
        "\"last_disconnect_reason_name\":\"%s\","
        "\"last_disconnect_reason_code\":%" PRIu8 ","
        "\"retry_count\":%" PRIu32 ","
        "\"seconds_until_retry\":%" PRIu32 ","
        "\"manual_reconnect_permitted\":%s,"
        "\"service_entry_permitted\":%s,"
        "\"factory_reset_pending\":%s,"
        "\"operator_action_required\":%s,"
        "\"timer_cancel_failure\":\"%s\","
        "\"timer_cancel_error\":\"%s\","
        "\"operator_guidance\":\"%s\"},"
        "\"broker\":{\"running\":%s,\"stopped\":%s,"
        "\"active_clients\":%" PRId32 ","
        "\"observable\":%s},"
        "\"commissioned\":%s"
        "}",
        machine_json,
        auth_mode_str, auth_owner_str,
        auth_snap.generation,
        net_mode_str,
        net_snap.sta_connected ? "true" : "false",
        net_snap.sta_ip_acquired ? "true" : "false",
        net_snap.ap_started ? "true" : "false",
        net_snap.sta_ip_address,
        argus_net_mgr_get_sta_state_name(net_snap.sta_state),
        argus_net_mgr_get_wifi_apply_state_name(net_snap.apply_state),
        category_str,
        reason_name,
        net_snap.last_disconnect_reason,
        net_snap.consecutive_failures,
        net_snap.seconds_until_retry,
        net_snap.manual_reconnect_permitted ? "true" : "false",
        service_entry_permitted ? "true" : "false",
        net_snap.factory_reset_pending ? "true" : "false",
        net_snap.action_required ? "true" : "false",
        timer_cancel_failure,
        esp_err_to_name(net_snap.last_service_cancel_error),
        operator_guidance,
        net_snap.mqtt_broker_running ? "true" : "false",
        net_snap.mqtt_broker_stopped ? "true" : "false",
        (broker_err == ESP_OK) ? broker_obs.active_client_count : (int32_t)0,
        (broker_err == ESP_OK) ? "true" : "false",
        commissioned ? "true" : "false");

    if (len < 0 || (size_t)len >= sizeof(buf)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"response buffer overflow\"}");
        return ESP_OK;
    }

    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* ── GET /api/identity handler ───────────────────────────────────── */

/**
 * @brief Returns non-secret device identity as JSON.
 *
 * Includes: hardware UID, client_id, unit_id, device_name, model,
 * firmware version, service SSID.
 *
 * Does NOT include: passwords, AP credentials, MAC address raw bytes.
 */
static esp_err_t identity_get_handler(httpd_req_t *req)
{
    if (!require_access(req, ARGUS_PERMISSION_VIEW_STATUS, false, NULL)) {
        return ESP_OK;
    }

    if (req->method != HTTP_GET) {
        return send_method_not_allowed(req);
    }

    set_api_headers(req);

    argus_identity_t id;
    esp_err_t err = argus_identity_get(&id);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"identity unavailable\"}");
        return ESP_OK;
    }

    /* Escape all string fields for safe JSON embedding */
    char esc_uid[ARGUS_IDENTITY_UID_LEN * 2 + 1];
    char esc_client[ARGUS_IDENTITY_CLIENT_ID_MAX * 2 + 1];
    char esc_unit[ARGUS_IDENTITY_UNIT_ID_MAX * 2 + 1];
    char esc_name[ARGUS_IDENTITY_DEV_NAME_MAX * 2 + 1];
    char esc_model[ARGUS_IDENTITY_MODEL_MAX * 2 + 1];
    char esc_fw[ARGUS_IDENTITY_FW_VER_MAX * 2 + 1];
    char esc_ssid[ARGUS_IDENTITY_SERVICE_SSID_MAX * 2 + 1];

    json_escape(id.mac_uid, esc_uid, sizeof(esc_uid));
    json_escape(id.client_id, esc_client, sizeof(esc_client));
    json_escape(id.unit_id, esc_unit, sizeof(esc_unit));
    json_escape(id.device_name, esc_name, sizeof(esc_name));
    json_escape(id.device_model, esc_model, sizeof(esc_model));
    json_escape(id.fw_version, esc_fw, sizeof(esc_fw));
    json_escape(id.service_ssid, esc_ssid, sizeof(esc_ssid));

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{"
        "\"hardware_uid\":\"%s\","
        "\"client_id\":\"%s\","
        "\"unit_id\":\"%s\","
        "\"device_name\":\"%s\","
        "\"device_model\":\"%s\","
        "\"firmware_version\":\"%s\","
        "\"service_ssid\":\"%s\""
        "}",
        esc_uid, esc_client, esc_unit, esc_name,
        esc_model, esc_fw, esc_ssid);

    if (len < 0 || (size_t)len >= sizeof(buf)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"response buffer overflow\"}");
        return ESP_OK;
    }

    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* ── GET / — Embedded portal page ────────────────────────────────── */

/**
 * Portal JavaScript rendering safety:
 *
 * The row() function builds HTML strings for display. All device-supplied
 * and configuration-supplied values (identity fields, state names) are
 * rendered through the h() function which escapes HTML metacharacters
 * (&, <, >, ", ') to prevent DOM injection. This is necessary because
 * JSON escaping alone does not prevent HTML interpretation.
 *
 * The identity section uses textContent for firmware_version and
 * service_ssid in the subtitle, which is inherently DOM-safe.
 */
static esp_err_t change_password_handler(httpd_req_t *req)
{
    if (!require_page_access(req, 0U, NULL)) return ESP_OK;
    if (req->method != HTTP_GET) return send_method_not_allowed(req);
    argus_http_security_headers(req, true);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/commission");
    return httpd_resp_send(req, NULL, 0);
}

/* ── GET /api/logout handler ─────────────────────────────────────── */

static esp_err_t logout_handler(httpd_req_t *req)
{
    set_api_headers(req);
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "POST");
    return httpd_resp_sendstr(
        req, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
}

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        return send_method_not_allowed(req);
    }
    argus_http_access_result_t access =
        argus_http_security_ap_only(req, NULL);
    if (access != ARGUS_HTTP_ACCESS_OK) {
        return argus_http_security_send_error(req, access);
    }
    char token[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
    argus_principal_t principal;
    bool authenticated =
        argus_http_security_cookie(req, token) &&
        argus_session_manager_authenticate(token, &principal, NULL) == ESP_OK;
    memset(token, 0, sizeof(token));
    memset(&principal, 0, sizeof(principal));
    argus_http_security_headers(req, true);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location",
                       authenticated ? "/operate" : "/login");
    return httpd_resp_send(req, NULL, 0);
}

extern const uint8_t argus_controls_html_start[]
    asm("_binary_argus_controls_html_start");
extern const uint8_t argus_controls_html_end[]
    asm("_binary_argus_controls_html_end");

static size_t controls_page_length(void)
{
    size_t len = (size_t)(argus_controls_html_end - argus_controls_html_start);
    if (len > 0U && argus_controls_html_start[len - 1U] == '\0') len--;
    return len;
}

static esp_err_t controls_get_handler(httpd_req_t *req)
{
    if (!require_page_access(req, ARGUS_PERMISSION_VIEW_STATUS, NULL)) {
        return ESP_OK;
    }
    if (req->method != HTTP_GET) return send_method_not_allowed(req);

    argus_http_security_headers(req, true);

    return httpd_resp_send(req, (const char *)argus_controls_html_start,
                           (ssize_t)controls_page_length());
}

static esp_err_t controls_alias_handler(httpd_req_t *req)
{
    if (!require_page_access(req, ARGUS_PERMISSION_VIEW_STATUS, NULL)) {
        return ESP_OK;
    }
    argus_http_security_headers(req, true);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/operate");
    return httpd_resp_send(req, NULL, 0);
}

/* ── Phase 4B.2: Bounded HTTP body receiver ──────────────────────── */

/**
 * @brief Read complete HTTP request body with bounds checking.
 *
 * Uses content_len from the request, loops until all bytes are received,
 * rejects oversized bodies, and always NUL-terminates on success.
 *
 * @param req      HTTP request handle.
 * @param buf      Destination buffer (must have room for buf_size bytes including NUL).
 * @param buf_size Size of destination buffer.
 * @return Number of body bytes read on success (>= 0), or -1 on error.
 *         On error, an appropriate HTTP error response has already been sent.
 */
#define RECV_BODY_REJECTED       (-1)
#define RECV_BODY_CLOSE_SESSION  (-2)

static int recv_full_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    size_t content_len = req->content_len;
    if (content_len == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"empty_body\"}");
        return RECV_BODY_REJECTED;
    }
    if (content_len >= buf_size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "{\"error\":\"body_too_large\"}");
        return RECV_BODY_CLOSE_SESSION;
    }
    size_t received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_set_status(req, "408 Request Timeout");
                httpd_resp_sendstr(req, "{\"error\":\"recv_timeout\"}");
            } else {
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_sendstr(req, "{\"error\":\"recv_failed\"}");
            }
            return RECV_BODY_CLOSE_SESSION;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return (int)received;
}

static bool copy_json_string(
    const cJSON *object, const char *name,
    char *out, size_t capacity, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) return !required;
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    size_t length = strnlen(item->valuestring, capacity);
    if (length >= capacity) return false;
    memcpy(out, item->valuestring, length + 1U);
    return true;
}

static void zeroize_json_string_field(cJSON *root, const char *name)
{
    cJSON *item = root == NULL
        ? NULL
        : cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        argus_password_zeroize(
            item->valuestring, strlen(item->valuestring));
    }
}

static bool decode_config_request(
    const char *body, size_t body_len,
    argus_config_scope_t *out_scope,
    argus_config_fields_t *out_fields)
{
    if (body == NULL || out_scope == NULL || out_fields == NULL) return false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(
        body, body_len, &end, false);
    if (root == NULL || !cJSON_IsObject(root) ||
        end != body + body_len) {
        zeroize_json_string_field(root, "sta_pass");
        cJSON_Delete(root);
        return false;
    }
    cJSON *password_item =
        cJSON_GetObjectItemCaseSensitive(root, "sta_pass");
    cJSON *scope_item =
        cJSON_GetObjectItemCaseSensitive(root, "scope");
    if (!cJSON_IsString(scope_item) ||
        scope_item->valuestring == NULL) {
        zeroize_json_string_field(root, "sta_pass");
        cJSON_Delete(root);
        return false;
    }
    argus_config_scope_t scope =
        argus_config_overlay_parse_scope(scope_item->valuestring);
    if (scope == ARGUS_CONFIG_SCOPE_INVALID) {
        zeroize_json_string_field(root, "sta_pass");
        cJSON_Delete(root);
        return false;
    }
    size_t count = 0U;
    bool valid = true;
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        count++;
        if (item->string == NULL ||
            cJSON_GetObjectItemCaseSensitive(root, item->string) != item) {
            valid = false;
            break;
        }
        bool allowed = strcmp(item->string, "scope") == 0;
        if (scope == ARGUS_CONFIG_SCOPE_IDENTITY) {
            allowed |= strcmp(item->string, "client_id") == 0 ||
                       strcmp(item->string, "unit_id") == 0 ||
                       strcmp(item->string, "device_name") == 0;
        } else {
            allowed |= strcmp(item->string, "sta_ssid") == 0 ||
                       strcmp(item->string, "sta_pass") == 0;
        }
        if (!allowed) {
            valid = false;
            break;
        }
    }
    memset(out_fields, 0, sizeof(*out_fields));
    if (valid && scope == ARGUS_CONFIG_SCOPE_IDENTITY) {
        valid = count == 4U &&
            copy_json_string(
                root, "client_id", out_fields->client_id,
                sizeof(out_fields->client_id), true) &&
            copy_json_string(
                root, "unit_id", out_fields->unit_id,
                sizeof(out_fields->unit_id), true) &&
            copy_json_string(
                root, "device_name", out_fields->device_name,
                sizeof(out_fields->device_name), true);
        out_fields->has_client_id = valid;
        out_fields->has_unit_id = valid;
        out_fields->has_device_name = valid;
    } else if (valid) {
        valid = (count == 2U || count == 3U) &&
            copy_json_string(
                root, "sta_ssid", out_fields->sta_ssid,
                sizeof(out_fields->sta_ssid), true) &&
            copy_json_string(
                root, "sta_pass", out_fields->sta_pass,
                sizeof(out_fields->sta_pass), false);
        out_fields->has_sta_ssid = valid;
        out_fields->has_sta_pass = valid && password_item != NULL;
    }
    zeroize_json_string_field(root, "sta_pass");
    cJSON_Delete(root);
    if (valid) *out_scope = scope;
    return valid;
}

/* ── Phase 4B.2: Identity Config Page HTML ───────────────────────── */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(req, 0U, false, &security)) return ESP_OK;
    argus_permission_set_t permissions = security.principal.permissions;
    memset(&security, 0, sizeof(security));
    if ((permissions &
         (ARGUS_PERMISSION_MODIFY_IDENTITY |
          ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK |
          ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG)) == 0U) {
        return argus_http_security_send_error(
            req, ARGUS_HTTP_ACCESS_FORBIDDEN);
    }
    if (req->method != HTTP_GET) return send_method_not_allowed(req);
    set_api_headers(req);

    argus_config_payload_t cfg;
    bool has_cfg = false;
    esp_err_t err = argus_nvs_config_get_effective(&cfg, &has_cfg);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"config_read_failed\"}");
        return ESP_OK;
    }

    bool id_prov = (cfg.provisioned_flags & ARGUS_CFG_PROVISIONED_IDENTITY) != 0;
    bool pass_set = (strlen(cfg.sta_pass) > 0);

    char esc_client[ARGUS_CFG_CLIENT_ID_MAX * 2 + 1];
    char esc_unit[ARGUS_CFG_UNIT_ID_MAX * 2 + 1];
    char esc_name[ARGUS_CFG_DEV_NAME_MAX * 2 + 1];
    char esc_ssid[ARGUS_CFG_STA_SSID_MAX * 2 + 1];
    json_escape(cfg.client_id, esc_client, sizeof(esc_client));
    json_escape(cfg.unit_id, esc_unit, sizeof(esc_unit));
    json_escape(cfg.device_name, esc_name, sizeof(esc_name));
    json_escape(cfg.sta_ssid, esc_ssid, sizeof(esc_ssid));

    /* Zero password immediately after determining if it was set */
    memset(cfg.sta_pass, 0, sizeof(cfg.sta_pass));

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"client_id\":\"%s\",\"unit_id\":\"%s\",\"device_name\":\"%s\","
        "\"sta_ssid\":\"%s\",\"sta_pass_set\":%s,\"identity_provisioned\":%s}",
        esc_client, esc_unit, esc_name, esc_ssid,
        pass_set ? "true" : "false",
        id_prov ? "true" : "false");

    if (len < 0 || (size_t)len >= sizeof(buf)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"response_overflow\"}");
        return ESP_OK;
    }
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* ── Phase 4B.2: POST /api/config/save handler ───────────────────── */

static esp_err_t config_save_handler(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(req, 0U, true, &security)) return ESP_OK;
    argus_permission_set_t permissions = security.principal.permissions;
    memset(&security, 0, sizeof(security));
    if (req->method != HTTP_POST) return send_method_not_allowed(req);
    set_api_headers(req);

    /* Mode gate: config writes only in provisioning/service/discoverable modes */
    argus_network_mode_t mode = argus_net_mgr_get_mode();
    if (mode != ARGUS_NET_MODE_UNCOMMISSIONED_AP &&
        mode != ARGUS_NET_MODE_SERVICE_AP_ONLY &&
        mode != ARGUS_NET_MODE_AP_DISCOVERABLE) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\":\"config_write_not_allowed_in_current_mode\"}");
        return ESP_OK;
    }

    /* Read request body */
    char body[512];
    int body_len = recv_full_body(req, body, sizeof(body));
    if (body_len == RECV_BODY_CLOSE_SESSION) return ESP_FAIL;
    if (body_len < 0) return ESP_OK;

    argus_config_scope_t scope = ARGUS_CONFIG_SCOPE_INVALID;
    argus_config_fields_t fields;
    bool decoded = decode_config_request(
        body, (size_t)body_len, &scope, &fields);
    memset(body, 0, sizeof(body));
    if (!decoded) {
        memset(&fields, 0, sizeof(fields));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req, "{\"error\":\"invalid_request\"}");
    }
    argus_permission_set_t required =
        scope == ARGUS_CONFIG_SCOPE_IDENTITY
            ? ARGUS_PERMISSION_MODIFY_IDENTITY
            : ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK;
    if ((permissions & required) != required) {
        memset(&fields, 0, sizeof(fields));
        return argus_http_security_send_error(
            req, ARGUS_HTTP_ACCESS_FORBIDDEN);
    }

    /* Load current effective config as base for overlay */
    argus_config_payload_t cfg;
    bool has_cfg = false;
    esp_err_t err = argus_nvs_config_get_effective(&cfg, &has_cfg);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"config_read_failed\"}");
        return ESP_OK;
    }

    /* Apply overlay via production seam */
    argus_config_payload_t out_cfg;
    argus_config_overlay_result_t overlay = argus_config_overlay_apply(&cfg, scope, &fields, &out_cfg);

    /* Zero sensitive fields from the parsed input */
    memset(fields.sta_pass, 0, sizeof(fields.sta_pass));

    if (!overlay.success) {
        httpd_resp_set_status(req, "400 Bad Request");
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg),
            "{\"error\":\"%s\",\"message\":\"%s\"}",
            overlay.error_code ? overlay.error_code : "overlay_failed",
            overlay.error_message ? overlay.error_message : "Configuration overlay failed");
        /* Identity lock is 403, not 400 */
        if (overlay.error_code && strcmp(overlay.error_code, "identity_locked") == 0) {
            httpd_resp_set_status(req, "403 Forbidden");
        }
        httpd_resp_sendstr(req, errmsg);
        return ESP_OK;
    }

    /* Validate the combined payload */
    err = argus_nvs_config_validate(&out_cfg);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"validation_failed\","
            "\"message\":\"Combined configuration failed validation\"}");
        memset(out_cfg.sta_pass, 0, sizeof(out_cfg.sta_pass));
        return ESP_OK;
    }

    /* Commit to NVS */
    err = argus_nvs_config_commit(&out_cfg);
    memset(out_cfg.sta_pass, 0, sizeof(out_cfg.sta_pass));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config commit failed: %s (%d)", esp_err_to_name(err), err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"commit_failed\"}");
        return ESP_OK;
    }

    if (scope == ARGUS_CONFIG_SCOPE_WIFI) {
        if (mode == ARGUS_NET_MODE_SERVICE_AP_ONLY) {
            httpd_resp_sendstr(req, "{\"status\":\"saved\",\"restart_required\":true,\"apply_queued\":false,\"message\":\"Wi-Fi configuration saved. Runtime apply is suppressed during Local Service; exit service to restart with the new configuration.\"}");
            return ESP_OK;
        }
        argus_net_event_t evt = { .type = ARGUS_NET_EVT_APPLY_WIFI_CONFIG };
        esp_err_t post_err = argus_net_mgr_post_event(&evt);
        if (post_err == ESP_OK) {
            httpd_resp_sendstr(req, "{\"status\":\"saved\",\"restart_required\":false,\"apply_queued\":true,\"message\":\"Wi-Fi configuration saved. Reconnection has started. Remain connected to the Service AP and refresh the dashboard to observe progress.\"}");
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"saved\",\"restart_required\":true,\"apply_queued\":false,\"message\":\"Configuration saved but runtime apply failed (queue full). Restart required.\"}");
        }
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"saved\",\"restart_required\":true,\"apply_queued\":false}");
    }
    return ESP_OK;
}

/* ── Phase 4B.2: Config page handlers ────────────────────────────── */

static esp_err_t identity_page_handler(httpd_req_t *req)
{
    if (!require_page_access(req, ARGUS_PERMISSION_MODIFY_IDENTITY,
                             NULL)) return ESP_OK;
    if (req->method != HTTP_GET) return send_method_not_allowed(req);
    argus_http_security_headers(req, true);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/commission");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t wifi_page_handler(httpd_req_t *req)
{
    if (!require_page_access(req, ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK,
                             NULL)) return ESP_OK;
    if (req->method != HTTP_GET) return send_method_not_allowed(req);
    argus_http_security_headers(req, true);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/commission");
    return httpd_resp_send(req, NULL, 0);
}

/* ── Phase 4B.2: POST /api/restart handler ───────────────────────── */


/* ── POST /api/network/reconnect handler ─────────────────────────── */

static esp_err_t reconnect_post_handler(httpd_req_t *req)
{
    if (!require_access(req, ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK,
                        true, NULL)) return ESP_OK;
    if (req->method != HTTP_POST) return send_method_not_allowed(req);

    set_api_headers(req);

    esp_err_t err = argus_net_mgr_request_manual_reconnect();
    if (err == ESP_OK) {
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_sendstr(req, "{\"status\":\"accepted\"}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"manual reconnect not permitted when uncommissioned\"}");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"manual reconnect conflict in current state\"}");
    } else if (err == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"queue full\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"internal error requesting reconnect\"}");
    }

    return ESP_OK;
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    if (!require_access(req, ARGUS_PERMISSION_MANAGE_FIRMWARE,
                        true, NULL)) return ESP_OK;
    if (req->method != HTTP_POST) return send_method_not_allowed(req);
    set_api_headers(req);

    esp_err_t err = argus_net_mgr_request_restart();
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"motion_active\","
            "\"message\":\"Cannot restart while motion is active or machine is in fault state\"}");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg),
            "{\"error\":\"restart_failed\",\"detail\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, errmsg);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"status\":\"restart_initiated\"}");
    return ESP_OK;
}

/* ── Phase 4B.3: Service Entry/Exit Handlers ─────────────────────── */

/**
 * @brief POST /api/service/enter — Request transition to LOCAL_SERVICE/BROWSER
 *
 * Mode gate: AP_DISCOVERABLE or UNCOMMISSIONED_AP (or already SERVICE_AP_ONLY
 * for idempotency).
 *
 * A1 DEADLOCK AVOIDANCE: argus_net_mgr_request_service() calls
 * argus_http_server_stop() internally. Calling it from within an httpd
 * handler would deadlock (httpd_stop waits for active handlers, but
 * this handler IS the active handler). Instead, we validate preconditions,
 * send the HTTP response, then post a SERVICE_REQUEST event to the net_mgr
 * task queue. The net_mgr task executes the transition from its own context.
 *
 * Per A2, once the event is posted the transition runs to completion
 * regardless of browser disconnect. The browser can poll /api/status
 * after reconnecting to the AP to verify the transition completed.
 */
static esp_err_t service_enter_handler(httpd_req_t *req)
{
    if (req->method != HTTP_POST) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_sendstr(req, "{\"error\":\"method_not_allowed\"}");
        return ESP_OK;
    }

    if (!require_access(req, ARGUS_PERMISSION_REQUEST_AUTHORITY,
                        true, NULL)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    argus_net_snapshot_t net_snap;
    argus_authority_snapshot_t auth_snap;
    argus_net_event_t evt = {0};
    argus_svc_policy_result_t pol;
    if (argus_net_mgr_evaluate_service_entry(
            ARGUS_AUTH_OWNER_BROWSER, &net_snap, &auth_snap, &evt, &pol) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"reason\":\"snapshot_failed\"}");
        return ESP_OK;
    }

    if (pol == ARGUS_SVC_POLICY_IDEMPOTENT) {
        httpd_resp_sendstr(req,
            "{\"status\":\"ok\",\"mode\":\"SERVICE_AP_ONLY\","
            "\"owner\":\"BROWSER\",\"note\":\"already_in_service\"}");
        return ESP_OK;
    } else if (pol == ARGUS_SVC_POLICY_TRANSITION_IN_PROGRESS) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"code\":409,\"reason\":\"transition_in_progress\"}");
        return ESP_OK;
    } else if (pol == ARGUS_SVC_POLICY_REJECT_MODE) {
        httpd_resp_set_status(req, "409 Conflict");
        char resp[160];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"error\",\"code\":409,\"reason\":\"mode_not_eligible\","
            "\"current_mode\":\"%s\"}", argus_net_mgr_get_mode_name(net_snap.mode));
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    } else if (pol != ARGUS_SVC_POLICY_OK) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"reason\":\"policy_rejected\"}");
        return ESP_OK;
    }

    /* Post the event — net_mgr task will execute the transition.
     * The HTTP server is stopped during the transition. The design is safe
     * from deadlock because the transition work occurs in a different task
     * context (net_mgr) and the HTTP server lifecycle is not stopped
     * recursively by its own handler. */
    esp_err_t post_err = argus_net_mgr_post_event(&evt);
    if (post_err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"error\",\"code\":503,\"reason\":\"queue_full\","
            "\"detail\":\"%s\"}", esp_err_to_name(post_err));
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    /* 202 Accepted — transition is in progress, browser should poll /api/status */
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req,
        "{\"status\":\"accepted\",\"action\":\"service_entry_requested\","
        "\"note\":\"Poll /api/status after reconnecting to verify transition\"}");

    return ESP_OK;
}

/**
 * @brief POST /api/service/exit — Revoke browser authority, reboot
 *
 * Mode gate: SERVICE_AP_ONLY with LOCAL_SERVICE/BROWSER authority.
 *
 * A1 DEADLOCK AVOIDANCE: Same pattern as service_enter. The handler
 * validates preconditions, sends the response, then posts a SERVICE_EXIT
 * event. The net_mgr task executes the controlled stop + reboot.
 *
 * The browser will lose connection after the response is sent because
 * the device reboots — this is expected and documented behavior.
 */
static esp_err_t service_exit_handler(httpd_req_t *req)
{
    if (req->method != HTTP_POST) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_sendstr(req, "{\"error\":\"method_not_allowed\"}");
        return ESP_OK;
    }

    if (!require_access(req, ARGUS_PERMISSION_REQUEST_AUTHORITY,
                        true, NULL)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* Check current state for policy */
    argus_net_snapshot_t net_snap;
    if (argus_net_mgr_get_snapshot(&net_snap) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"reason\":\"snapshot_failed\"}");
        return ESP_OK;
    }

    argus_authority_snapshot_t auth_snap;
    argus_authority_mgr_get_snapshot(&auth_snap);

    argus_net_event_t evt;
    argus_svc_policy_result_t pol = argus_service_policy_evaluate_exit(&net_snap, &auth_snap, &evt);

    if (pol == ARGUS_SVC_POLICY_REJECT_MODE) {
        httpd_resp_set_status(req, "403 Forbidden");
        char resp[160];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"error\",\"code\":403,\"reason\":\"not_in_service_mode\","
            "\"current_mode\":\"%s\"}", argus_net_mgr_get_mode_name(net_snap.mode));
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    } else if (pol == ARGUS_SVC_POLICY_REJECT_AUTHORITY) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"code\":403,\"reason\":\"not_browser_owner\"}");
        return ESP_OK;
    } else if (pol != ARGUS_SVC_POLICY_OK) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"reason\":\"policy_rejected\"}");
        return ESP_OK;
    }

    /* Post the event — net_mgr task will execute the exit + reboot */
    esp_err_t post_err = argus_net_mgr_post_event(&evt);
    if (post_err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"error\",\"code\":503,\"reason\":\"queue_full\","
            "\"detail\":\"%s\"}", esp_err_to_name(post_err));
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    /* 202 Accepted — device will reboot shortly after this response */
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req,
        "{\"status\":\"accepted\",\"action\":\"service_exit_requested\","
        "\"note\":\"Device will reboot. Reconnect to verify.\"}");

    return ESP_OK;
}

/* ── URI handler registrations ───────────────────────────────────── */


/* Phase 4B.4: POST /api/command */

typedef struct {
    httpd_req_t *req;
} command_body_recv_ctx_t;

static int command_body_recv(void *ctx, uint8_t *dst, size_t max_len)
{
    command_body_recv_ctx_t *recv_ctx = (command_body_recv_ctx_t *)ctx;
    int received = httpd_req_recv(recv_ctx->req, (char *)dst, max_len);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) {
        return ARGUS_BROWSER_BODY_RECV_TIMEOUT;
    }
    return received;
}

static esp_err_t command_get_network_snapshot(void *ctx, argus_net_snapshot_t *out_snap)
{
    (void)ctx;
    return argus_net_mgr_get_snapshot(out_snap);
}

static esp_err_t command_get_authority_snapshot(void *ctx,
                                                argus_authority_snapshot_t *out_snap)
{
    (void)ctx;
    return argus_authority_mgr_get_snapshot(out_snap);
}

static esp_err_t command_dispatch(void *ctx, const argus_command_envelope_t *envelope)
{
    (void)ctx;
    return argus_cmd_router_dispatch(envelope);
}

static esp_err_t send_command_response(
    httpd_req_t *req,
    argus_browser_command_endpoint_result_t endpoint_result)
{
    argus_browser_command_http_response_t response;
    if (!argus_browser_command_response_for(endpoint_result, &response)) {
        (void)argus_browser_command_response_for(ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR,
                                                 &response);
    }

    set_api_headers(req);
    httpd_resp_set_status(req, response.status_line);
    return httpd_resp_sendstr(req, response.json_body);
}

static argus_permission_set_t command_capability(argus_cmd_type_t type)
{
    switch (type) {
        case ARGUS_CMD_TYPE_ESTOP:
            return ARGUS_PERMISSION_SOFTWARE_ESTOP;
        case ARGUS_CMD_TYPE_RESET_ESTOP:
            return ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP;
        case ARGUS_CMD_TYPE_SET_TARGET:
        case ARGUS_CMD_TYPE_START:
        case ARGUS_CMD_TYPE_STOP_NORMAL:
        case ARGUS_CMD_TYPE_UNLOCK:
        case ARGUS_CMD_TYPE_RECOVER:
            return ARGUS_PERMISSION_MOTION;
        default:
            return 0U;
    }
}

static esp_err_t command_post_handler(httpd_req_t *req)
{
    /* Set security headers before authentication so 401 responses are no-store. */
    set_api_headers(req);
    argus_http_security_context_t security;
    if (!require_access(req, 0U, true, &security)) {
        return ESP_OK;
    }

    uint8_t body[ARGUS_BROWSER_COMMAND_MAX_BODY_LEN];
    size_t body_len = 0U;
    command_body_recv_ctx_t recv_ctx = { .req = req };
    argus_browser_body_receive_result_t receive_result =
        argus_browser_command_receive_body(req->content_len,
                                           command_body_recv,
                                           &recv_ctx,
                                           body,
                                           sizeof(body),
                                           &body_len);
    argus_browser_command_receive_disposition_t disposition;
    if (!argus_browser_command_receive_disposition(receive_result, &disposition)) {
        disposition = (argus_browser_command_receive_disposition_t){
            .continue_request = false,
            .close_session = true,
            .response_result = ARGUS_BROWSER_ENDPOINT_INTERNAL_ERROR,
            .handler_result_after_response = ESP_FAIL,
        };
    }
    if (!disposition.continue_request) {
        if (disposition.close_session) {
            httpd_resp_set_hdr(req, "Connection", "close");
        }
        esp_err_t response_err = send_command_response(req, disposition.response_result);
        /* ESP-IDF closes and cleans up the session when a URI handler returns
         * an error after httpd_req_recv() failed or framing is uncertain. */
        return response_err != ESP_OK ? response_err
                                      : disposition.handler_result_after_response;
    }

    argus_browser_command_request_t decoded;
    argus_browser_command_decode_result_t decode_result =
        argus_browser_command_decode(body, body_len, &decoded);
    if (decode_result == ARGUS_BROWSER_CMD_DECODE_OK && decoded.is_valid) {
        argus_permission_set_t required =
            command_capability(decoded.command_type);
        if (required == 0U ||
            argus_authorization_require(&security.principal, required) !=
                ARGUS_AUTHZ_ALLOW) {
            memset(&security, 0, sizeof(security));
            return argus_http_security_send_error(
                req, ARGUS_HTTP_ACCESS_FORBIDDEN);
        }
    }

    const argus_browser_command_endpoint_ops_t ops = {
        .get_network_snapshot = command_get_network_snapshot,
        .get_authority_snapshot = command_get_authority_snapshot,
        .dispatch = command_dispatch,
        .ctx = NULL,
    };
    argus_browser_command_endpoint_outcome_t outcome;
    argus_browser_command_endpoint_result_t endpoint_result =
        argus_browser_command_endpoint_process(true, body, body_len, &ops, &outcome);

    memset(&security, 0, sizeof(security));
    return send_command_response(req, endpoint_result);
}

typedef struct {
    httpd_req_t *req;
} factory_reset_recv_ctx_t;

static int factory_reset_body_recv(void *ctx, uint8_t *dst, size_t max_len)
{
    factory_reset_recv_ctx_t *recv_ctx = (factory_reset_recv_ctx_t *)ctx;
    int received = httpd_req_recv(recv_ctx->req, (char *)dst, max_len);
    return received == HTTPD_SOCK_ERR_TIMEOUT
               ? ARGUS_FACTORY_RESET_BODY_RECV_TIMEOUT
               : received;
}

static esp_err_t send_factory_reset_response(httpd_req_t *req, int status_code)
{
    const char *status = "500 Internal Server Error";
    const char *body = "{\"ok\":false,\"error\":\"internal_error\"}";
    switch (status_code) {
        case 202:
            status = "202 Accepted";
            body = "{\"ok\":true,\"status\":\"factory_reset_accepted\","
                   "\"note\":\"Configuration reset and reboot pending. Reconnect to the Service AP and verify authoritative status.\"}";
            break;
        case 400:
            status = "400 Bad Request";
            body = "{\"ok\":false,\"error\":\"invalid_request\"}";
            break;
        case 403:
            status = "403 Forbidden";
            body = "{\"ok\":false,\"error\":\"factory_reset_not_admitted\"}";
            break;
        case 409:
            status = "409 Conflict";
            body = "{\"ok\":false,\"error\":\"lifecycle_conflict\"}";
            break;
        case 415:
            status = "415 Unsupported Media Type";
            body = "{\"ok\":false,\"error\":\"unsupported_media_type\"}";
            break;
        case 503:
            status = "503 Service Unavailable";
            body = "{\"ok\":false,\"error\":\"queue_unavailable\"}";
            break;
        default:
            break;
    }
    set_api_headers(req);
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    set_api_headers(req);
    if (req->method != HTTP_POST) return send_factory_reset_response(req, 400);
    argus_http_security_context_t security;
    if (!require_access(req, ARGUS_PERMISSION_COMMISSION,
                        true, &security)) return ESP_OK;
    bool recent = argus_session_manager_recently_reauthenticated(
        security.session_index);
    memset(&security, 0, sizeof(security));
    if (!recent) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(
            req,
            "{\"ok\":false,\"error\":\"recent_reauthentication_required\"}");
    }

    char content_type[48];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK ||
        !argus_factory_reset_content_type_valid(content_type)) {
        return send_factory_reset_response(req, 415);
    }

    uint8_t body[ARGUS_FACTORY_RESET_MAX_BODY_LEN];
    size_t body_len = 0U;
    factory_reset_recv_ctx_t recv_ctx = {.req = req};
    argus_factory_reset_receive_result_t receive_result =
        argus_factory_reset_receive_body(req->content_len,
                                         factory_reset_body_recv, &recv_ctx,
                                         body, sizeof(body), &body_len);
    argus_factory_reset_receive_disposition_t disposition;
    if (!argus_factory_reset_receive_disposition(receive_result, &disposition)) {
        disposition = (argus_factory_reset_receive_disposition_t){
            .close_session = true,
            .status_code = 500,
            .handler_result_after_response = ESP_FAIL,
        };
    }
    if (!disposition.continue_request) {
        if (disposition.close_session) {
            httpd_resp_set_hdr(req, "Connection", "close");
        }
        esp_err_t response_err =
            send_factory_reset_response(req, disposition.status_code);
        return response_err != ESP_OK ? response_err
                                      : disposition.handler_result_after_response;
    }

    if (argus_factory_reset_decode(body, body_len) !=
        ARGUS_FACTORY_RESET_DECODE_OK) {
        return send_factory_reset_response(req, 400);
    }

    esp_err_t err = argus_net_mgr_request_factory_reset();
    if (err == ESP_OK) return send_factory_reset_response(req, 202);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_factory_reset_response(req, 409);
    if (err == ESP_ERR_INVALID_STATE) return send_factory_reset_response(req, 403);
    if (err == ESP_ERR_NO_MEM) return send_factory_reset_response(req, 503);
    return send_factory_reset_response(req, 500);
}

extern const uint8_t argus_login_html_start[]
    asm("_binary_argus_login_html_start");
extern const uint8_t argus_login_html_end[]
    asm("_binary_argus_login_html_end");

#define LOGIN_BODY_MAX 512U

static esp_err_t receive_bounded_json(
    httpd_req_t *req, uint8_t *body, size_t capacity, size_t *out_len)
{
    if (req == NULL || body == NULL || out_len == NULL ||
        req->content_len == 0U || req->content_len >= capacity) {
        if (req != NULL && req->content_len >= capacity) {
            httpd_resp_set_hdr(req, "Connection", "close");
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_ERR_INVALID_ARG;
    }
    size_t received = 0U;
    while (received < req->content_len) {
        int result = httpd_req_recv(
            req, (char *)body + received, req->content_len - received);
        if (result <= 0 ||
            (size_t)result > req->content_len - received) {
            httpd_resp_set_hdr(req, "Connection", "close");
            return result == HTTPD_SOCK_ERR_TIMEOUT
                       ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        received += (size_t)result;
    }
    body[received] = 0U;
    *out_len = received;
    return ESP_OK;
}

static bool decode_login(
    const uint8_t *body, size_t body_len,
    char username[ARGUS_LOGIN_NAME_MAX + 1U],
    uint8_t password[ARGUS_PASSWORD_INPUT_MAX + 1U],
    size_t *password_len)
{
    if (body == NULL || body_len == 0U ||
        username == NULL || password == NULL || password_len == NULL) {
        return false;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(
        (const char *)body, body_len, &end, false);
    if (root == NULL || !cJSON_IsObject(root) ||
        end != (const char *)body + body_len) {
        zeroize_json_string_field(root, "password");
        cJSON_Delete(root);
        return false;
    }
    cJSON *username_item = NULL;
    cJSON *password_item = NULL;
    size_t fields = 0U;
    bool valid = true;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        fields++;
        if (item->string == NULL) {
            valid = false;
        } else if (strcmp(item->string, "username") == 0) {
            if (username_item != NULL) valid = false;
            username_item = item;
        } else if (strcmp(item->string, "password") == 0) {
            if (password_item != NULL) valid = false;
            password_item = item;
        } else {
            valid = false;
        }
    }
    if (!valid || fields != 2U ||
        !cJSON_IsString(username_item) ||
        !cJSON_IsString(password_item) ||
        username_item->valuestring == NULL ||
        password_item->valuestring == NULL) {
        zeroize_json_string_field(root, "password");
        cJSON_Delete(root);
        return false;
    }
    size_t username_len = strnlen(
        username_item->valuestring, ARGUS_LOGIN_NAME_MAX + 1U);
    size_t secret_len = strnlen(
        password_item->valuestring, ARGUS_PASSWORD_INPUT_MAX + 1U);
    if (username_len == 0U || username_len > ARGUS_LOGIN_NAME_MAX ||
        secret_len == 0U || secret_len > ARGUS_PASSWORD_INPUT_MAX) {
        zeroize_json_string_field(root, "password");
        cJSON_Delete(root);
        return false;
    }
    memcpy(username, username_item->valuestring, username_len + 1U);
    memcpy(password, password_item->valuestring, secret_len);
    password[secret_len] = 0U;
    *password_len = secret_len;
    zeroize_json_string_field(root, "password");
    cJSON_Delete(root);
    return true;
}

static esp_err_t login_get_handler(httpd_req_t *req)
{
    argus_http_security_headers(req, true);
    argus_http_access_result_t access =
        argus_http_security_ap_only(req, NULL);
    if (access != ARGUS_HTTP_ACCESS_OK) {
        return argus_http_security_send_error(req, access);
    }
    size_t length =
        (size_t)(argus_login_html_end - argus_login_html_start);
    if (length > 0U && argus_login_html_start[length - 1U] == 0U) {
        length--;
    }
    return httpd_resp_send(
        req, (const char *)argus_login_html_start, (ssize_t)length);
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    set_api_headers(req);
    uint32_t peer_key;
    argus_http_access_result_t access =
        argus_http_security_ap_only(req, &peer_key);
    if (access == ARGUS_HTTP_ACCESS_OK) {
        access = argus_http_security_origin(req);
    }
    if (access != ARGUS_HTTP_ACCESS_OK) {
        return argus_http_security_send_error(req, access);
    }
    if (!argus_http_security_json_content_type(req)) {
        return argus_http_security_send_error(
            req, ARGUS_HTTP_ACCESS_CONTENT_TYPE);
    }
    uint8_t body[LOGIN_BODY_MAX];
    size_t body_len = 0U;
    esp_err_t receive_err = receive_bounded_json(
        req, body, sizeof(body), &body_len);
    if (receive_err != ESP_OK) {
        memset(body, 0, sizeof(body));
        httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"invalid_request\"}");
        return receive_err == ESP_ERR_TIMEOUT || receive_err == ESP_FAIL
                   ? ESP_FAIL : ESP_OK;
    }
    char username[ARGUS_LOGIN_NAME_MAX + 1U];
    uint8_t password[ARGUS_PASSWORD_INPUT_MAX + 1U];
    size_t password_len = 0U;
    bool decoded = decode_login(
        body, body_len, username, password, &password_len);
    memset(body, 0, sizeof(body));
    if (!decoded) {
        memset(username, 0, sizeof(username));
        memset(password, 0, sizeof(password));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"invalid_request\"}");
    }
    argus_login_outcome_t outcome = argus_auth_service_authenticate(
        peer_key, username, password, password_len);
    memset(username, 0, sizeof(username));
    memset(password, 0, sizeof(password));
    if (outcome.result != ARGUS_LOGIN_SUCCESS) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (s_last_login_failure_audit_us == 0U ||
            now_us - s_last_login_failure_audit_us >=
            UINT64_C(60000000)) {
            argus_security_store_status_t status = {0};
            (void)argus_security_store_get_status(&status);
            (void)argus_security_audit_append(
                outcome.result == ARGUS_LOGIN_THROTTLED
                    ? ARGUS_AUDIT_LOGIN_THROTTLED
                    : ARGUS_AUDIT_LOGIN_FAILURE,
                ARGUS_AUDIT_OUTCOME_REJECTED,
                ARGUS_PRINCIPAL_NONE, "anonymous", "login", "SOFTAP",
                outcome.result == ARGUS_LOGIN_THROTTLED
                    ? "throttled" : "generic_failure",
                status.security_epoch, false);
            s_last_login_failure_audit_us = now_us;
        }
        if (outcome.result == ARGUS_LOGIN_THROTTLED) {
            char retry[12];
            snprintf(retry, sizeof(retry), "%lu",
                     (unsigned long)outcome.retry_after_s);
            httpd_resp_set_hdr(req, "Retry-After", retry);
            httpd_resp_set_status(req, "429 Too Many Requests");
        } else if (outcome.result == ARGUS_LOGIN_BUSY) {
            httpd_resp_set_status(req, "503 Service Unavailable");
        } else if (outcome.result == ARGUS_LOGIN_STORE_UNAVAILABLE) {
            httpd_resp_set_status(req, "503 Service Unavailable");
        } else {
            httpd_resp_set_status(req, "401 Unauthorized");
        }
        memset(&outcome, 0, sizeof(outcome));
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"authentication_failed\"}");
    }
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    ESP_LOGI(TAG, "Login path stack high-water margin: %u bytes",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) *
                        sizeof(StackType_t)));
#endif
    if (argus_security_audit_append(
            ARGUS_AUDIT_LOGIN_SUCCESS, ARGUS_AUDIT_OUTCOME_SUCCESS,
            (uint8_t)outcome.principal.type,
            outcome.principal.identifier, "session", "SOFTAP",
            "authenticated", outcome.principal.security_epoch,
            true) != ESP_OK) {
        memset(&outcome, 0, sizeof(outcome));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"audit_unavailable\"}");
    }
    argus_session_credentials_t credentials;
    esp_err_t session_err = argus_session_manager_create(
        &outcome.principal, &credentials);
    memset(&outcome, 0, sizeof(outcome));
    if (session_err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"session_unavailable\"}");
    }
    char cookie[128];
    int cookie_len = snprintf(
        cookie, sizeof(cookie),
        ARGUS_SESSION_COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict",
        credentials.token);
    char response[320];
    const char *type = credentials.principal.type ==
                               ARGUS_PRINCIPAL_CONSOLE
                           ? "CONSOLE" : "HUMAN";
    int response_len = snprintf(
        response, sizeof(response),
        "{\"ok\":true,\"principal\":{\"id\":\"%s\",\"type\":\"%s\","
        "\"level\":%u},\"csrf\":\"%s\"}",
        credentials.principal.identifier, type,
        (unsigned)credentials.principal.level, credentials.csrf);
    if (cookie_len <= 0 || (size_t)cookie_len >= sizeof(cookie) ||
        response_len <= 0 || (size_t)response_len >= sizeof(response)) {
        (void)argus_session_manager_revoke_token(credentials.token);
        memset(&credentials, 0, sizeof(credentials));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"internal_error\"}");
    }
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    esp_err_t send_err = httpd_resp_send(req, response, response_len);
    memset(cookie, 0, sizeof(cookie));
    memset(response, 0, sizeof(response));
    memset(&credentials, 0, sizeof(credentials));
    return send_err;
}

static esp_err_t session_get_handler(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(req, 0U, false, &security)) return ESP_OK;
    char csrf[ARGUS_SESSION_TOKEN_HEX_LEN + 1U];
    if (!argus_session_manager_get_csrf(
            security.session_index, csrf)) {
        memset(&security, 0, sizeof(security));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"internal_error\"}");
    }
    set_api_headers(req);
    static const struct {
        argus_permission_set_t bit;
        const char *name;
    } CAPABILITIES[] = {
        {ARGUS_PERMISSION_VIEW_STATUS, "view_status"},
        {ARGUS_PERMISSION_REQUEST_AUTHORITY, "request_authority"},
        {ARGUS_PERMISSION_MOTION, "motion"},
        {ARGUS_PERMISSION_SOFTWARE_ESTOP, "software_estop"},
        {ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP, "reset_software_estop"},
        {ARGUS_PERMISSION_ACK_ALARMS, "ack_alarms"},
        {ARGUS_PERMISSION_MANAGE_USERS, "manage_users"},
        {ARGUS_PERMISSION_MANAGE_ROLES, "manage_roles"},
        {ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS, "manage_client_admins"},
        {ARGUS_PERMISSION_ENROLL_MACHINES, "enroll_machines"},
        {ARGUS_PERMISSION_REVOKE_MACHINES, "revoke_machines"},
        {ARGUS_PERMISSION_VIEW_AUDIT, "view_audit"},
        {ARGUS_PERMISSION_MANAGE_NETWORK, "manage_network"},
        {ARGUS_PERMISSION_CHANGE_AP_SECRET, "change_ap_secret"},
        {ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK, "manage_client_network"},
        {ARGUS_PERMISSION_MANAGE_MQTT, "manage_mqtt"},
        {ARGUS_PERMISSION_MODIFY_IDENTITY, "modify_identity"},
        {ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG,
         "modify_protected_config"},
        {ARGUS_PERMISSION_COMMISSION, "commission"},
        {ARGUS_PERMISSION_CALIBRATE, "calibrate"},
        {ARGUS_PERMISSION_MANAGE_FIRMWARE, "manage_firmware"},
        {ARGUS_PERMISSION_INVOKE_RECOVERY, "invoke_recovery"},
        {ARGUS_PERMISSION_FULL_SECURITY_RESET, "full_security_reset"},
    };
    const char *role = "Unknown";
    switch (security.principal.level) {
        case ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL:
            role = "Argus Personnel";
            break;
        case ARGUS_SECURITY_LEVEL_CLIENT_ADMIN:
            role = "Client Administrator";
            break;
        case ARGUS_SECURITY_LEVEL_SUPERVISOR:
            role = "Supervisor";
            break;
        case ARGUS_SECURITY_LEVEL_OPERATOR:
            role = "Operator";
            break;
        case ARGUS_SECURITY_LEVEL_VIEWER:
            role = "Viewer";
            break;
        default:
            break;
    }
    char display[ARGUS_SECURITY_DISPLAY_MAX + 1U] = "Argus Console";
    if (security.principal.type == ARGUS_PRINCIPAL_HUMAN) {
        argus_security_human_record_t human = {0};
        if (argus_security_directory_find_id(
                security.principal.identifier, &human, NULL) == ESP_OK) {
            strlcpy(display, human.display_name, sizeof(display));
        }
        argus_password_zeroize(&human, sizeof(human));
    }
    char escaped_display[ARGUS_SECURITY_DISPLAY_MAX * 2U + 1U];
    json_escape(display, escaped_display, sizeof(escaped_display));
    char response[1400];
    int length = snprintf(
        response, sizeof(response),
        "{\"ok\":true,\"principal\":{\"id\":\"%s\",\"type\":\"%s\","
        "\"display_name\":\"%s\",\"role\":\"%s\",\"level\":%u},"
        "\"csrf\":\"%s\",\"session\":{\"idle_timeout_s\":900,"
        "\"absolute_timeout_s\":28800},\"security_recovery\":%s,"
        "\"capabilities\":[",
        security.principal.identifier,
        security.principal.type == ARGUS_PRINCIPAL_CONSOLE
            ? "CONSOLE" : "HUMAN",
        escaped_display, role, (unsigned)security.principal.level, csrf,
        argus_security_store_get_recovery_state() ==
                ARGUS_SECURITY_RECOVERY_REQUESTED ? "true" : "false");
    for (size_t i = 0U;
         length > 0 &&
         i < sizeof(CAPABILITIES) / sizeof(CAPABILITIES[0]); ++i) {
        if ((security.principal.permissions & CAPABILITIES[i].bit) == 0U) {
            continue;
        }
        int added = snprintf(
            response + length, sizeof(response) - (size_t)length,
            "%s\"%s\"", response[length - 1] == '[' ? "" : ",",
            CAPABILITIES[i].name);
        if (added <= 0 ||
            (size_t)added >= sizeof(response) - (size_t)length) {
            length = -1;
            break;
        }
        length += added;
    }
    if (length > 0) {
        int added = snprintf(
            response + length, sizeof(response) - (size_t)length,
            "]}");
        if (added <= 0 ||
            (size_t)added >= sizeof(response) - (size_t)length) {
            length = -1;
        } else {
            length += added;
        }
    }
    memset(csrf, 0, sizeof(csrf));
    memset(display, 0, sizeof(display));
    memset(escaped_display, 0, sizeof(escaped_display));
    memset(&security, 0, sizeof(security));
    if (length <= 0 || (size_t)length >= sizeof(response)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"internal_error\"}");
    }
    return httpd_resp_send(req, response, length);
}

static esp_err_t logout_post_handler(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(req, 0U, true, &security)) return ESP_OK;
    bool revoked =
        argus_session_manager_revoke_token(security.session_token);
    (void)argus_security_audit_append(
        ARGUS_AUDIT_LOGOUT,
        revoked ? ARGUS_AUDIT_OUTCOME_SUCCESS
                : ARGUS_AUDIT_OUTCOME_FAILED,
        (uint8_t)security.principal.type,
        security.principal.identifier, "session", "SOFTAP",
        revoked ? "logout" : "session_missing",
        security.principal.security_epoch, false);
    memset(&security, 0, sizeof(security));
    httpd_resp_set_hdr(
        req, "Set-Cookie",
        ARGUS_SESSION_COOKIE_NAME
        "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    set_api_headers(req);
    return httpd_resp_sendstr(
        req, revoked ? "{\"ok\":true}" :
                       "{\"ok\":false,\"error\":\"session_not_found\"}");
}

static bool current_login(
    const argus_principal_t *principal,
    char out[ARGUS_LOGIN_NAME_MAX + 1U])
{
    if (principal->type == ARGUS_PRINCIPAL_CONSOLE) {
        strlcpy(out, "argus", ARGUS_LOGIN_NAME_MAX + 1U);
        return true;
    }
    if (principal->type != ARGUS_PRINCIPAL_HUMAN) return false;
    argus_security_human_record_t human = {0};
    bool found = argus_security_directory_find_id(
        principal->identifier, &human, NULL) == ESP_OK;
    if (found) {
        strlcpy(out, human.login, ARGUS_LOGIN_NAME_MAX + 1U);
    }
    argus_password_zeroize(&human, sizeof(human));
    return found;
}

static bool parse_password_object(
    const uint8_t *body,
    size_t body_len,
    bool changing,
    uint8_t current[ARGUS_PASSWORD_INPUT_MAX + 1U],
    size_t *current_len,
    uint8_t replacement[ARGUS_PASSWORD_INPUT_MAX + 1U],
    size_t *replacement_len)
{
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(
        (const char *)body, body_len, &end, false);
    if (root == NULL || !cJSON_IsObject(root) ||
        end != (const char *)body + body_len) {
        zeroize_json_string_field(root, "current_password");
        zeroize_json_string_field(root, "new_password");
        zeroize_json_string_field(root, "confirm_password");
        cJSON_Delete(root);
        return false;
    }
    cJSON *current_item = NULL;
    cJSON *new_item = NULL;
    cJSON *confirm_item = NULL;
    size_t fields = 0U;
    bool valid = true;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        fields++;
        if (item->string == NULL) {
            valid = false;
        } else if (strcmp(item->string, "current_password") == 0) {
            if (current_item != NULL) valid = false;
            current_item = item;
        } else if (changing &&
                   strcmp(item->string, "new_password") == 0) {
            if (new_item != NULL) valid = false;
            new_item = item;
        } else if (changing &&
                   strcmp(item->string, "confirm_password") == 0) {
            if (confirm_item != NULL) valid = false;
            confirm_item = item;
        } else {
            valid = false;
        }
    }
    size_t expected_fields = changing ? 3U : 1U;
    if (!valid || fields != expected_fields ||
        !cJSON_IsString(current_item) ||
        current_item->valuestring == NULL ||
        (changing && (!cJSON_IsString(new_item) ||
                      !cJSON_IsString(confirm_item) ||
                      new_item->valuestring == NULL ||
                      confirm_item->valuestring == NULL))) {
        zeroize_json_string_field(root, "current_password");
        zeroize_json_string_field(root, "new_password");
        zeroize_json_string_field(root, "confirm_password");
        cJSON_Delete(root);
        return false;
    }
    size_t old_len = strnlen(
        current_item->valuestring, ARGUS_PASSWORD_INPUT_MAX + 1U);
    size_t new_len = changing
        ? strnlen(new_item->valuestring, ARGUS_PASSWORD_INPUT_MAX + 1U)
        : 0U;
    valid = old_len > 0U && old_len <= ARGUS_PASSWORD_INPUT_MAX;
    if (changing) {
        valid = valid && new_len <= ARGUS_PASSWORD_INPUT_MAX &&
            strcmp(new_item->valuestring,
                   confirm_item->valuestring) == 0 &&
            argus_auth_new_password_valid(
                (const uint8_t *)new_item->valuestring, new_len);
    }
    if (valid) {
        memcpy(current, current_item->valuestring, old_len);
        current[old_len] = 0U;
        *current_len = old_len;
        if (changing) {
            memcpy(replacement, new_item->valuestring, new_len);
            replacement[new_len] = 0U;
            *replacement_len = new_len;
        }
    }
    if (current_item != NULL && cJSON_IsString(current_item) &&
        current_item->valuestring != NULL) {
        argus_password_zeroize(
            current_item->valuestring,
            strlen(current_item->valuestring));
    }
    if (new_item != NULL && cJSON_IsString(new_item) &&
        new_item->valuestring != NULL) {
        argus_password_zeroize(
            new_item->valuestring, strlen(new_item->valuestring));
    }
    if (confirm_item != NULL && cJSON_IsString(confirm_item) &&
        confirm_item->valuestring != NULL) {
        argus_password_zeroize(
            confirm_item->valuestring,
            strlen(confirm_item->valuestring));
    }
    cJSON_Delete(root);
    return valid;
}

static bool reauthenticate_principal(
    const argus_http_security_context_t *security,
    const uint8_t *password,
    size_t password_len)
{
    char login[ARGUS_LOGIN_NAME_MAX + 1U] = {0};
    if (!current_login(&security->principal, login)) return false;
    argus_login_outcome_t outcome = argus_auth_service_authenticate(
        security->peer_key, login, password, password_len);
    bool valid = outcome.result == ARGUS_LOGIN_SUCCESS &&
        outcome.principal.type == security->principal.type &&
        strcmp(outcome.principal.identifier,
               security->principal.identifier) == 0;
    argus_password_zeroize(login, sizeof(login));
    argus_password_zeroize(&outcome, sizeof(outcome));
    return valid;
}

static esp_err_t reauth_post_handler(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(req, 0U, true, &security)) return ESP_OK;
    set_api_headers(req);
    uint8_t body[LOGIN_BODY_MAX] = {0};
    size_t body_len = 0U;
    esp_err_t receive_err = receive_bounded_json(
        req, body, sizeof(body), &body_len);
    uint8_t current[ARGUS_PASSWORD_INPUT_MAX + 1U] = {0};
    uint8_t unused[ARGUS_PASSWORD_INPUT_MAX + 1U] = {0};
    size_t current_len = 0U;
    size_t unused_len = 0U;
    bool valid = receive_err == ESP_OK &&
        parse_password_object(
            body, body_len, false, current, &current_len,
            unused, &unused_len) &&
        reauthenticate_principal(&security, current, current_len);
    argus_password_zeroize(body, sizeof(body));
    argus_password_zeroize(current, sizeof(current));
    argus_password_zeroize(unused, sizeof(unused));
    if (valid) {
        argus_session_manager_mark_reauthenticated(
            security.session_index);
    }
    memset(&security, 0, sizeof(security));
    if (receive_err == ESP_ERR_TIMEOUT || receive_err == ESP_FAIL) {
        httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"invalid_request\"}");
        return ESP_FAIL;
    }
    if (receive_err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"invalid_request\"}");
    }
    if (!valid) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"authentication_failed\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t change_own_password_post_handler(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(req, 0U, true, &security)) return ESP_OK;
    set_api_headers(req);
    uint8_t body[LOGIN_BODY_MAX] = {0};
    size_t body_len = 0U;
    esp_err_t receive_err = receive_bounded_json(
        req, body, sizeof(body), &body_len);
    uint8_t current[ARGUS_PASSWORD_INPUT_MAX + 1U] = {0};
    uint8_t replacement[ARGUS_PASSWORD_INPUT_MAX + 1U] = {0};
    size_t current_len = 0U;
    size_t replacement_len = 0U;
    bool valid = receive_err == ESP_OK &&
        parse_password_object(
            body, body_len, true, current, &current_len,
            replacement, &replacement_len) &&
        reauthenticate_principal(&security, current, current_len);
    argus_password_zeroize(body, sizeof(body));
    argus_password_zeroize(current, sizeof(current));
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (valid && argus_security_audit_append(
            ARGUS_AUDIT_PASSWORD_CHANGED,
            ARGUS_AUDIT_OUTCOME_SUCCESS,
            (uint8_t)security.principal.type,
            security.principal.identifier,
            security.principal.identifier, "SOFTAP",
            "self_change_reserved",
            security.principal.security_epoch, true) == ESP_OK) {
        if (security.principal.type == ARGUS_PRINCIPAL_CONSOLE) {
            argus_password_verifier_t verifier = {0};
            err = argus_auth_service_create_verifier(
                replacement, replacement_len, &verifier);
            if (err == ESP_OK) {
                err = argus_security_store_set_console_verifier(
                    &verifier, true);
            }
            argus_password_zeroize(&verifier, sizeof(verifier));
            if (err == ESP_OK) {
                (void)argus_session_manager_revoke_all();
            }
        } else {
            err = argus_security_admin_replace_own_password(
                &security.principal, replacement, replacement_len);
        }
    }
    argus_password_zeroize(replacement, sizeof(replacement));
    memset(&security, 0, sizeof(security));
    if (receive_err == ESP_ERR_TIMEOUT || receive_err == ESP_FAIL) {
        httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"invalid_request\"}");
        return ESP_FAIL;
    }
    if (receive_err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"invalid_request\"}");
    }
    if (!valid) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"authentication_failed\"}");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"password_change_failed\"}");
    }
    httpd_resp_set_hdr(
        req, "Set-Cookie",
        ARGUS_SESSION_COOKIE_NAME
        "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return httpd_resp_sendstr(
        req, "{\"ok\":true,\"fresh_login_required\":true}");
}

extern const uint8_t argus_commission_html_start[]
    asm("_binary_argus_commission_html_start");
extern const uint8_t argus_commission_html_end[]
    asm("_binary_argus_commission_html_end");

static esp_err_t commission_get_handler(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_page_access(req, 0U, &security)) return ESP_OK;
    argus_permission_set_t administration =
        ARGUS_PERMISSION_MANAGE_USERS |
        ARGUS_PERMISSION_MANAGE_ROLES |
        ARGUS_PERMISSION_VIEW_AUDIT |
        ARGUS_PERMISSION_MANAGE_NETWORK |
        ARGUS_PERMISSION_CHANGE_AP_SECRET |
        ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK |
        ARGUS_PERMISSION_MODIFY_IDENTITY |
        ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG |
        ARGUS_PERMISSION_COMMISSION |
        ARGUS_PERMISSION_CALIBRATE |
        ARGUS_PERMISSION_MANAGE_FIRMWARE |
        ARGUS_PERMISSION_INVOKE_RECOVERY |
        ARGUS_PERMISSION_FULL_SECURITY_RESET;
    if ((security.principal.permissions & administration) == 0U) {
        memset(&security, 0, sizeof(security));
        return argus_http_security_send_error(
            req, ARGUS_HTTP_ACCESS_FORBIDDEN);
    }
    memset(&security, 0, sizeof(security));
    argus_http_security_headers(req, true);
    size_t length = (size_t)(
        argus_commission_html_end - argus_commission_html_start);
    if (length > 0U && argus_commission_html_start[length - 1U] == 0U) {
        length--;
    }
    return httpd_resp_send(
        req, (const char *)argus_commission_html_start,
        (ssize_t)length);
}

static const httpd_uri_t uri_login_get = {
    .uri = "/login",
    .method = HTTP_GET,
    .handler = login_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_login_post = {
    .uri = "/api/auth/login",
    .method = HTTP_POST,
    .handler = login_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_session_get = {
    .uri = "/api/auth/session",
    .method = HTTP_GET,
    .handler = session_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_logout_post = {
    .uri = "/api/auth/logout",
    .method = HTTP_POST,
    .handler = logout_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_reauth_post = {
    .uri = "/api/auth/reauth",
    .method = HTTP_POST,
    .handler = reauth_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_change_own_password_post = {
    .uri = "/api/auth/change-password",
    .method = HTTP_POST,
    .handler = change_own_password_post_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_operate = {
    .uri = "/operate",
    .method = HTTP_GET,
    .handler = controls_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_commission = {
    .uri = "/commission",
    .method = HTTP_GET,
    .handler = commission_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_status = {
    .uri       = "/api/status",
    .method    = HTTP_GET,
    .handler   = status_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_identity = {
    .uri       = "/api/identity",
    .method    = HTTP_GET,
    .handler   = identity_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_portal = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = portal_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_controls = {
    .uri       = "/controls",
    .method    = HTTP_GET,
    .handler   = controls_alias_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_change_password = {
    .uri       = "/change-password",
    .method    = HTTP_GET,
    .handler   = change_password_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_logout = {
    .uri       = "/api/logout",
    .method    = HTTP_GET,
    .handler   = logout_handler,
    .user_ctx  = NULL
};

/* Phase 4B.2 URI handler registrations */

static const httpd_uri_t uri_config_get = {
    .uri       = "/api/config",
    .method    = HTTP_GET,
    .handler   = config_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_config_save = {
    .uri       = "/api/config/save",
    .method    = HTTP_POST,
    .handler   = config_save_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_identity_page = {
    .uri       = "/config/identity",
    .method    = HTTP_GET,
    .handler   = identity_page_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_wifi_page = {
    .uri       = "/config/wifi",
    .method    = HTTP_GET,
    .handler   = wifi_page_handler,
    .user_ctx  = NULL
};


static const httpd_uri_t uri_reconnect = {
    .uri       = "/api/network/reconnect",
    .method    = HTTP_POST,
    .handler   = reconnect_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_restart = {
    .uri       = "/api/restart",
    .method    = HTTP_POST,
    .handler   = restart_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_service_enter = {
    .uri       = "/api/service/enter",
    .method    = HTTP_POST,
    .handler   = service_enter_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_service_exit = {
    .uri       = "/api/service/exit",
    .method    = HTTP_POST,
    .handler   = service_exit_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_command = {
    .uri       = ARGUS_BROWSER_COMMAND_URI,
    .method    = HTTP_POST,
    .handler   = command_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_factory_reset = {
    .uri       = ARGUS_FACTORY_RESET_URI,
    .method    = HTTP_POST,
    .handler   = factory_reset_post_handler,
    .user_ctx  = NULL
};

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
bool argus_http_test_command_registration(void)
{
    return strcmp(uri_command.uri, ARGUS_BROWSER_COMMAND_URI) == 0 &&
           uri_command.method == HTTP_POST &&
           uri_command.handler == command_post_handler;
}

bool argus_http_test_factory_reset_registration(void)
{
    return strcmp(uri_factory_reset.uri, ARGUS_FACTORY_RESET_URI) == 0 &&
           uri_factory_reset.method == HTTP_POST &&
           uri_factory_reset.handler == factory_reset_post_handler;
}

bool argus_http_test_controls_registration(void)
{
    return strcmp(uri_controls.uri, "/controls") == 0 &&
           uri_controls.method == HTTP_GET &&
           uri_controls.handler == controls_alias_handler;
}

const char *argus_http_test_controls_page(size_t *out_len)
{
    if (out_len != NULL) *out_len = controls_page_length();
    return (const char *)argus_controls_html_start;
}

const char *argus_http_test_commission_page(size_t *out_len)
{
    size_t length = (size_t)(
        argus_commission_html_end - argus_commission_html_start);
    if (length > 0U && argus_commission_html_start[length - 1U] == 0U) {
        length--;
    }
    if (out_len != NULL) *out_len = length;
    return (const char *)argus_commission_html_start;
}

bool argus_http_test_decode_login(const uint8_t *body, size_t body_len)
{
    char username[ARGUS_LOGIN_NAME_MAX + 1U] = {0};
    uint8_t password[ARGUS_PASSWORD_INPUT_MAX + 1U] = {0};
    size_t password_len = 0U;
    bool result = decode_login(
        body, body_len, username, password, &password_len);
    argus_password_zeroize(username, sizeof(username));
    argus_password_zeroize(password, sizeof(password));
    return result;
}

uint64_t argus_http_test_command_capability(uint32_t command_type)
{
    return command_capability((argus_cmd_type_t)command_type);
}
#endif

/* ── Lifecycle implementation ────────────────────────────────────── */

esp_err_t argus_http_server_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lifecycle_mutex = xSemaphoreCreateMutex();
    if (s_lifecycle_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create lifecycle mutex");
        return ESP_ERR_NO_MEM;
    }

    s_server = NULL;
    s_initialized = true;
    ESP_LOGI(TAG, "HTTP server initialized (max_conn=%d)", HTTP_MAX_CONNECTIONS);
    return ESP_OK;
}

/* ── Private lifecycle helper ────────────────────────────────────── */

/**
 * @brief Stop the HTTP server while s_lifecycle_mutex is already held.
 *
 * Unified stop rule: s_server is cleared only after confirmed successful
 * httpd_stop(). On failure, the handle is preserved to prevent unverified
 * duplicate starts. Both the public stop() and the startup rollback path
 * use this function.
 *
 * @return ESP_OK on success, or the httpd_stop() error on failure.
 */
static esp_err_t stop_server_locked(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_stop failed: %s (%d). Handle preserved.",
                 esp_err_to_name(err), err);
        return err;
    }

    s_server = NULL;
    return ESP_OK;
}

esp_err_t argus_http_server_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);

    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running (idempotent start)");
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers   = 32;
    config.max_open_sockets   = HTTP_MAX_CONNECTIONS;
    config.recv_wait_timeout  = HTTP_RECV_TIMEOUT_S;
    config.send_wait_timeout  = HTTP_SEND_TIMEOUT_S;
    config.stack_size         = HTTP_STACK_SIZE;
    config.lru_purge_enable   = true;  /* Evict oldest connection if at max */
    config.server_port        = 80;
    /* Human handlers enforce the SoftAP socket boundary in middleware. */

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s (%d)", esp_err_to_name(err), err);
        s_server = NULL;
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_FAIL;
    }

    /* Register URI handlers — roll back on any failure */
    esp_err_t reg_err;
    reg_err = httpd_register_uri_handler(s_server, &uri_login_get);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_login_post);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_session_get);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_logout_post);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_reauth_post);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(
        s_server, &uri_change_own_password_post);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_operate);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_commission);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_status);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_identity);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_portal);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_controls);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_change_password);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_logout);
    if (reg_err != ESP_OK) goto rollback;
    /* Phase 4B.2 endpoints */
    reg_err = httpd_register_uri_handler(s_server, &uri_config_get);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_config_save);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_identity_page);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_wifi_page);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_restart);
    if (reg_err != ESP_OK) goto rollback;
    /* Phase 4B.3 endpoints */
    reg_err = httpd_register_uri_handler(s_server, &uri_service_enter);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_service_exit);
        if (reg_err != ESP_OK) goto rollback;

    reg_err = httpd_register_uri_handler(s_server, &uri_reconnect);
    if (reg_err != ESP_OK) goto rollback;
    /* Phase 4B.4 endpoint */
    reg_err = httpd_register_uri_handler(s_server, &uri_command);
    if (reg_err != ESP_OK) goto rollback;
    /* Phase 4B.6 endpoint */
    reg_err = httpd_register_uri_handler(s_server, &uri_factory_reset);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = argus_security_http_register(s_server);
    if (reg_err != ESP_OK) goto rollback;

    ESP_LOGI(TAG, "HTTP server started on port 80 (max_conn=%d)", HTTP_MAX_CONNECTIONS);
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_OK;

rollback:
    ESP_LOGE(TAG, "URI handler registration failed: %s. Rolling back server.",
             esp_err_to_name(reg_err));
    /* Use stop_server_locked() — applies the same lifecycle rule:
     * clear s_server only on confirmed stop; preserve on failure. */
    esp_err_t stop_err = stop_server_locked();
    if (stop_err != ESP_OK) {
        ESP_LOGE(TAG, "Rollback stop also failed: %s. Server in unknown state.",
                 esp_err_to_name(stop_err));
    }
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_FAIL;
}

esp_err_t argus_http_server_stop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    esp_err_t err = stop_server_locked();
    if (err == ESP_OK && s_server == NULL) {
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    xSemaphoreGive(s_lifecycle_mutex);
    return err;
}

bool argus_http_server_is_running(void)
{
    if (!s_initialized) return false;

    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    bool running = (s_server != NULL);
    xSemaphoreGive(s_lifecycle_mutex);
    return running;
}
