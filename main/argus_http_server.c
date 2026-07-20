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
 * 7. SERVICE AP INTERFACE ENFORCEMENT (DEFERRED)
 *    Socket-level interface filtering is not possible on ESP32 lwip (returns
 *    0.0.0.0 for getpeername/getsockname). STA-side filtering deferred to
 *    Phase 4B.3. Risk mitigated by credential protection (see 8).
 *
 * 8. CREDENTIAL PROTECTION
 *    All endpoints require HTTP Basic Auth. Default credentials: admin/admin.
 *    On first access, the portal shows a password-change form. The password
 *    is stored in NVS (namespace "argus_portal"). After password change,
 *    the browser re-prompts with the new credentials. This protects against
 *    unauthorized LAN access in APSTA mode.
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

#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "argus_http";

/* ── Lifecycle state ─────────────────────────────────────────────── */

static SemaphoreHandle_t s_lifecycle_mutex = NULL;
static httpd_handle_t    s_server = NULL;
static bool              s_initialized = false;

#define HTTP_MAX_CONNECTIONS  4
#define HTTP_RECV_TIMEOUT_S   5
#define HTTP_SEND_TIMEOUT_S   5
#define HTTP_STACK_SIZE       6144

#define PORTAL_NVS_NAMESPACE  "argus_portal"
#define PORTAL_NVS_KEY_PW     "pw"
#define PORTAL_NVS_KEY_PW_SET "pw_set"
#define PORTAL_DEFAULT_USER   "admin"
#define PORTAL_DEFAULT_PASS   "admin"
#define PORTAL_MAX_PW_LEN     64
#define PORTAL_MIN_PW_LEN     4

/* ── Portal Credential Storage ───────────────────────────────── */

/**
 * @brief Get the current portal password from NVS.
 * @param out_pw  Buffer to receive password (must be PORTAL_MAX_PW_LEN+1).
 * @param out_is_default  Set to true if password has never been changed.
 *
 * Error semantics:
 *   - Namespace genuinely absent (first boot): return bootstrap default.
 *   - pw_set key genuinely absent: return bootstrap default.
 *   - pw_set == 1: return stored replacement password.
 *   - Any NVS error other than not-found: fail closed (empty password,
 *     is_default = false). The bootstrap credential is NOT re-enabled
 *     by NVS read errors.
 */
static void portal_get_credentials(char *out_pw, bool *out_is_default)
{
    out_pw[0] = '\0';
    *out_is_default = true;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PORTAL_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace genuinely absent = first-boot device, bootstrap default */
        strncpy(out_pw, PORTAL_DEFAULT_PASS, PORTAL_MAX_PW_LEN);
        out_pw[PORTAL_MAX_PW_LEN] = '\0';
        return;
    }
    if (err != ESP_OK) {
        /* NVS error (corruption, not initialized, etc.) — fail closed */
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s — fail closed",
                 PORTAL_NVS_NAMESPACE, esp_err_to_name(err));
        *out_is_default = false;
        return;
    }

    /* Read the pw_set marker */
    uint8_t pw_set = 0;
    err = nvs_get_u8(nvs, PORTAL_NVS_KEY_PW_SET, &pw_set);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Key genuinely absent = password never changed, bootstrap default */
        strncpy(out_pw, PORTAL_DEFAULT_PASS, PORTAL_MAX_PW_LEN);
        out_pw[PORTAL_MAX_PW_LEN] = '\0';
        nvs_close(nvs);
        return;
    }
    if (err != ESP_OK) {
        /* NVS read error on pw_set — fail closed, do NOT re-enable default */
        ESP_LOGE(TAG, "nvs_get_u8(%s) failed: %s — fail closed",
                 PORTAL_NVS_KEY_PW_SET, esp_err_to_name(err));
        *out_is_default = false;
        nvs_close(nvs);
        return;
    }

    if (pw_set == 1) {
        *out_is_default = false;
        /* Read the stored replacement password */
        size_t len = PORTAL_MAX_PW_LEN + 1;
        if (nvs_get_str(nvs, PORTAL_NVS_KEY_PW, out_pw, &len) != ESP_OK) {
            ESP_LOGE(TAG, "pw_set=1 but password read failed — fail closed");
            out_pw[0] = '\0';
        }
    } else {
        /* pw_set exists but != 1 — treat as first-boot, bootstrap default */
        strncpy(out_pw, PORTAL_DEFAULT_PASS, PORTAL_MAX_PW_LEN);
        out_pw[PORTAL_MAX_PW_LEN] = '\0';
    }
    nvs_close(nvs);
}

/**
 * @brief Store a new portal password in NVS.
 */
static esp_err_t portal_set_password(const char *new_pw)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PORTAL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, PORTAL_NVS_KEY_PW, new_pw);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, PORTAL_NVS_KEY_PW_SET, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}



/* ── JSON helpers now in argus_json.h/c ─────────────────────── */

/* ── HTTP Basic Auth ───────────────────────────────────────── */

/**
 * @brief Check HTTP Basic Auth credentials.
 *
 * Parses the Authorization header, base64-decodes it, and compares
 * against "admin:<stored_password>". Returns true if authenticated.
 * On failure, sends 401 with WWW-Authenticate header.
 */
static bool check_auth(httpd_req_t *req)
{
    char auth_hdr[128];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr));
    if (err != ESP_OK) {
        goto unauthorized;
    }

    /* Expect "Basic <base64>" */
    if (strncmp(auth_hdr, "Basic ", 6) != 0) {
        goto unauthorized;
    }

    const char *b64 = auth_hdr + 6;
    unsigned char decoded[128];
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                    (const unsigned char *)b64, strlen(b64));
    if (ret != 0 || decoded_len == 0) {
        goto unauthorized;
    }
    decoded[decoded_len] = '\0';

    /* Format: "username:password" */
    char *colon = strchr((char *)decoded, ':');
    if (colon == NULL) {
        goto unauthorized;
    }
    *colon = '\0';
    const char *username = (const char *)decoded;
    const char *password = colon + 1;

    if (strcmp(username, PORTAL_DEFAULT_USER) != 0) {
        goto unauthorized;
    }

    char stored_pw[PORTAL_MAX_PW_LEN + 1];
    bool is_default;
    portal_get_credentials(stored_pw, &is_default);

    /* Defense-in-depth: after password change, the hardcoded default
     * is permanently blocked regardless of NVS state */
    if (!is_default && strcmp(password, PORTAL_DEFAULT_PASS) == 0) {
        memset(stored_pw, 0, sizeof(stored_pw));
        memset(decoded, 0, sizeof(decoded));
        goto unauthorized;
    }

    bool match = (strcmp(password, stored_pw) == 0);
    memset(stored_pw, 0, sizeof(stored_pw));  /* clear from stack */
    memset(decoded, 0, sizeof(decoded));

    if (!match) {
        goto unauthorized;
    }

    return true;

unauthorized:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Argus Service Portal\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
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

/* Expose json_escape for pure testing in diagnostic builds */
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
void argus_http_test_json_escape(const char *src, char *dst, size_t dst_size)
{
    json_escape(src, dst, dst_size);
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
    if (!check_auth(req)) return ESP_OK;

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

    const char *machine_state_str = argus_state_mgr_get_state_name(state_snap.machine_state);
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

    /* Build JSON response */
    char buf[1536];
    int len = snprintf(buf, sizeof(buf),
        "{"
        "\"machine\":{\"state\":\"%s\","
        "\"target_rpm_milli\":%" PRId32 ","
        "\"applied_rpm_milli\":%" PRId32 ","
        "\"generated_rpm_milli\":%" PRId32 ","
        "\"driver_enabled\":%s,"
        "\"estop_latched\":%s,"
        "\"ramp_active\":%s},"
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
        "\"operator_action_required\":%s,"
        "\"timer_cancel_failure\":\"%s\","
        "\"timer_cancel_error\":\"%s\","
        "\"operator_guidance\":\"%s\"},"
        "\"broker\":{\"running\":%s,\"stopped\":%s,"
        "\"active_clients\":%" PRId32 ","
        "\"observable\":%s},"
        "\"commissioned\":%s"
        "}",
        machine_state_str,
        state_snap.configured_target_rpm_milli,
        state_snap.applied_rpm_milli,
        state_snap.generated_rpm_milli,
        state_snap.driver_enabled ? "true" : "false",
        state_snap.estop_latched ? "true" : "false",
        state_snap.ramp_active ? "true" : "false",
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
    if (!check_auth(req)) return ESP_OK;

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
static const char PORTAL_HTML[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Argus Controller</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "background:#0f1117;color:#e4e4e7;min-height:100vh;padding:16px}"
    "h1{font-size:1.25rem;font-weight:600;color:#a78bfa;margin-bottom:4px}"
    ".sub{font-size:0.75rem;color:#71717a;margin-bottom:16px}"
    ".card{background:#1a1b23;border:1px solid #27272a;border-radius:12px;"
    "padding:16px;margin-bottom:12px}"
    ".card h2{font-size:0.875rem;font-weight:500;color:#a1a1aa;margin-bottom:12px;"
    "text-transform:uppercase;letter-spacing:0.05em}"
    ".row{display:flex;justify-content:space-between;padding:6px 0;"
    "border-bottom:1px solid #27272a;font-size:0.875rem}"
    ".row:last-child{border-bottom:none}"
    ".label{color:#a1a1aa}.val{color:#e4e4e7;font-weight:500;text-align:right}"
    ".badge{display:inline-block;padding:2px 8px;border-radius:9999px;"
    "font-size:0.75rem;font-weight:600}"
    ".badge-ok{background:#064e3b;color:#34d399}"
    ".badge-warn{background:#451a03;color:#fbbf24}"
    ".badge-off{background:#27272a;color:#71717a}"
    ".badge-err{background:#450a0a;color:#f87171}"
    "#status-indicator{width:8px;height:8px;border-radius:50%;display:inline-block;"
    "margin-right:6px;background:#71717a}"
    ".refresh-btn{background:#7c3aed;color:#fff;border:none;border-radius:8px;"
    "padding:10px 20px;font-size:0.875rem;font-weight:500;cursor:pointer;"
    "width:100%;margin-top:8px;transition:background 0.15s}"
    ".refresh-btn:active{background:#6d28d9}"
    ".note{font-size:0.7rem;color:#52525b;margin-top:8px;font-style:italic}"
    "</style>"
    "</head>"
    "<body>"
    "<h1>&#9670; Argus Controller</h1>"
    "<div class=\"sub\" id=\"fw-ver\">Loading...</div>"
    "<div class=\"card\" id=\"card-machine\">"
    "<h2>Machine</h2>"
    "<div id=\"machine-rows\">Loading...</div>"
    "</div>"
    "<div class=\"card\" id=\"card-authority\">"
    "<h2>Authority</h2>"
    "<div id=\"authority-rows\">Loading...</div>"
    "</div>"
    "<div class=\"card\" id=\"card-network\">"
    "<h2>Network</h2>"
    "<div id=\"network-rows\">Loading...</div>"
    "</div>"
    "<div class=\"card\" id=\"card-identity\">"
    "<h2>Identity</h2>"
    "<div id=\"identity-rows\">Loading...</div>"
    "</div>"
    "<div id=\"service-controls\"></div>"
    "<button class=\"refresh-btn\" onclick=\"refresh()\">Refresh Status</button>"
    "<div style=\"display:flex;gap:8px;margin-top:8px\">"
    "<button class=\"refresh-btn\" style=\"background:#3f3f46\" onclick=\"window.location='/change-password'\">Change Password</button>"
    "<button class=\"refresh-btn\" style=\"background:#dc2626\" onclick=\"window.location='/api/logout'\">Log Out</button>"
    "</div>"
    "<div style=\"display:flex;gap:8px;margin-top:16px;flex-wrap:wrap\">"
    "<a href='/config/identity' style='flex:1;text-align:center;text-decoration:none;background:#3f3f46;color:#fff;border:none;border-radius:8px;padding:10px 20px;font-size:0.875rem;font-weight:500;cursor:pointer;transition:background 0.15s'>Identity</a>"
    "<a href='/config/wifi' style='flex:1;text-align:center;text-decoration:none;background:#3f3f46;color:#fff;border:none;border-radius:8px;padding:10px 20px;font-size:0.875rem;font-weight:500;cursor:pointer;transition:background 0.15s'>WiFi Config</a>"
    "<button onclick='doRestart()' style='flex:1;background:#dc2626;color:#fff;border:none;border-radius:8px;padding:10px 20px;font-size:0.875rem;font-weight:500;cursor:pointer;transition:background 0.15s'>Restart</button>"
    "</div>"
    "<div class=\"note\">Service portal. Generated speeds are not proof of physical shaft motion.</div>"
    "<script>"
    /* h() — HTML-escape to prevent DOM injection from device-supplied values */
    "function h(s){if(s==null)return'';\n"
    "return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')\n"
    ".replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;')}\n"
    "function row(l,v,c){return '<div class=\"row\"><span class=\"label\">'+h(l)+\n"
    "'</span><span class=\"val\">'+(c?'<span class=\"badge '+h(c)+'\">'+h(v)+'</span>':h(v))+'</span></div>'}\n"
    "function bc(s){var m={'RUNNING':'badge-ok','HOLDING':'badge-ok','UNLOCKED':'badge-warn',\n"
    "'STARTING':'badge-warn','DECELERATING':'badge-warn','EMERGENCY_STOPPED':'badge-err',\n"
    "'FAULTED':'badge-err','BOOTING':'badge-off','RECOVERING':'badge-warn'};\n"
    "return m[s]||'badge-off'}\n"
    "function nc(s){return s==='COMMISSIONED_STA'||s==='AP_DISCOVERABLE'?'badge-ok':\n"
    "s==='SERVICE_AP_ONLY'?'badge-warn':s==='NETWORK_FAULT'?'badge-err':'badge-off'}\n"
    "function fmt(v){return (v/1000).toFixed(1)+' RPM'}\n"
    "function refresh(){\n"
    "fetch('/api/status').then(r=>r.json()).then(d=>{\n"
    "var m=d.machine,a=d.authority,n=d.network;\n"
    "document.getElementById('machine-rows').innerHTML=\n"
    "row('State',m.state,bc(m.state))+\n"
    "row('Target Speed',fmt(m.target_rpm_milli))+\n"
    "row('Applied Speed',fmt(m.applied_rpm_milli))+\n"
    "row('Generated Speed',fmt(m.generated_rpm_milli))+\n"
    "row('Driver',m.driver_enabled?'Enabled':'Disabled',m.driver_enabled?'badge-ok':'badge-off')+\n"
    "row('E-Stop',m.estop_latched?'LATCHED':'Clear',m.estop_latched?'badge-err':'badge-ok')+\n"
    "row('Ramp',m.ramp_active?'Active':'Idle',m.ramp_active?'badge-warn':'badge-off');\n"
    "document.getElementById('authority-rows').innerHTML=\n"
    "row('Mode',a.mode)+\n"
    "row('Owner',a.owner)+\n"
    "row('Generation',a.generation);\n"
    "var sc=document.getElementById('service-controls');sc.innerHTML='';\n"
    "var nmode = n.mode || 'UNKNOWN';\n"
    "var nsta = n.sta_state || 'UNKNOWN';\n"
    "var ncat = n.last_error_category || 'NONE';\n"
    "document.getElementById('network-rows').innerHTML=\n"
    "row('Mode',nmode,nc(nmode))+\n"
    "row('STA State',nsta,nc(nsta))+\n"
    "row('Recovery',n.recovery_state||'IDLE',nc(n.recovery_state||'IDLE'))+\n"
    "row('STA Connected',n.sta_connected?'Yes':'No',n.sta_connected?'badge-ok':'badge-off')+\n"
    "row('AP Started',n.ap_started?'Yes':'No',n.ap_started?'badge-ok':'badge-off')+\n"
    "(n.sta_ip_acquired?row('STA IP',n.sta_ip_address||'UNKNOWN','badge-ok'):'')+\n"
    "row('Broker',!d.broker.observable?'Unobservable':d.broker.running?'Running':d.broker.stopped?'Stopped':'Not converged',d.broker.running?'badge-ok':d.broker.stopped?'badge-off':'badge-warn')+\n"
    "row('Commissioned',d.commissioned?'Yes':'No',d.commissioned?'badge-ok':'badge-warn')+\n"
    "(ncat!=='NONE'?row('Last Failure',ncat+' ('+(n.last_disconnect_reason_name||'')+')','badge-warn'):'')+\n"
    "((n.retry_count||0)>0?row('Failures',n.retry_count,'badge-err'):'')+\n"
    "((n.seconds_until_retry||0)>0?row('Retry In',n.seconds_until_retry+'s','badge-warn'):'')+\n"
    "row('Guidance',n.operator_guidance||'Unknown',n.operator_action_required?'badge-err':'badge-ok');\n"
    "if(n.manual_reconnect_permitted){\n"
    "sc.innerHTML+='<button id=\"btn-reconnect\" class=\"refresh-btn\" style=\"background:#3b82f6;margin-bottom:8px\" onclick=\"doReconnect()\">Reconnect Wi-Fi</button>';\n"
    "}\n"
    "if(n.service_entry_permitted){\n"
    "sc.innerHTML+='<button id=\"btn-enter\" class=\"refresh-btn\" style=\"background:#eab308;color:#000;margin-bottom:8px\" onclick=\"doSvcEnter()\">Enter Local Service</button>';\n"
    "}else if(nmode==='SERVICE_AP_ONLY'&&a.mode==='LOCAL_SERVICE'&&a.owner==='BROWSER'){\n"
    "sc.innerHTML+='<button id=\"btn-exit\" class=\"refresh-btn\" style=\"background:#10b981;margin-bottom:8px\" onclick=\"doSvcExit()\">Exit Local Service</button>';\n"
    "}\n"
    "}).catch(e=>{document.getElementById('machine-rows').innerHTML=\n"
    "row('Error','Failed to load status','badge-err')});\n"
    "fetch('/api/identity').then(r=>r.json()).then(d=>{\n"
    "document.getElementById('fw-ver').textContent=d.firmware_version+' | '+d.service_ssid;\n"
    "document.getElementById('identity-rows').innerHTML=\n"
    "row('Hardware UID',d.hardware_uid)+\n"
    "row('Client ID',d.client_id||'(not set)')+\n"
    "row('Unit ID',d.unit_id||'(not set)')+\n"
    "row('Device Name',d.device_name||'(not set)')+\n"
    "row('Model',d.device_model);\n"
    "}).catch(e=>{})}\n"
    "function doRestart(){\n"
    "if(!confirm('Restart the controller? Active connections will be lost.'))return;\n"
    "fetch('/api/restart',{method:'POST'}).then(r=>r.json()).then(d=>{\n"
    "if(d.status==='restart_initiated'){\n"
    "document.body.innerHTML='<div style=\"text-align:center;padding:40px;color:#a78bfa\"><h2>Restarting...</h2><p style=\"color:#a1a1aa;margin-top:8px\">The controller is restarting.</p></div>';\n"
    "}else{alert(d.message||d.error||'Restart failed')}\n"
    "}).catch(()=>alert('Connection lost'))}\n"
    "function doSvcEnter(){\n"
    "if(!confirm('Enter Local Service?\\n\\n- MQTT authority will be relinquished\\n- STA connectivity will be disabled\\n- Portal may disconnect briefly\\n- The pump will enter browser-owned local service after transition completes.'))return;\n"
    "var b=document.getElementById('btn-enter');if(b){b.disabled=true;b.textContent='Requesting...';}\n"
    "fetch('/api/service/enter',{method:'POST'}).then(r=>r.json()).then(d=>{\n"
    "if(d.status==='accepted'||d.status==='ok'){\n"
    "document.body.innerHTML='<div style=\"text-align:center;padding:40px;color:#a78bfa\"><h2>Transition Pending</h2><p style=\"color:#a1a1aa;margin-top:8px\">Service entry requested. You may temporarily lose connection. Please reconnect to the Service AP if necessary and wait for the dashboard to refresh.</p><button onclick=\"location.reload()\" class=\"refresh-btn\" style=\"margin-top:16px\">Reload Dashboard</button></div>';\n"
    "}else{alert(d.reason||d.error||'Request failed');if(b){b.disabled=false;b.textContent='Enter Local Service';}}\n"
    "}).catch(()=>{\n"
    "alert('Network error. The request status is unknown. Please reconnect to the Service AP and reload to verify /api/status.');\n"
    "if(b){b.disabled=false;b.textContent='Enter Local Service';}\n"
    "})}\n"
    "function doSvcExit(){\n"
    "if(!confirm('Exit Local Service?\\n\\nThe controller will safely exit local service and reboot. You will lose connection.'))return;\n"
    "var b=document.getElementById('btn-exit');if(b){b.disabled=true;b.textContent='Requesting...';}\n"
    "fetch('/api/service/exit',{method:'POST'}).then(r=>r.json()).then(d=>{\n"
    "if(d.status==='accepted'){\n"
    "document.body.innerHTML='<div style=\"text-align:center;padding:40px;color:#a78bfa\"><h2>Exiting Service...</h2><p style=\"color:#a1a1aa;margin-top:8px\">Device reboot requested. Connection will be lost.</p></div>';\n"
    "}else{alert(d.reason||d.error||'Request failed');if(b){b.disabled=false;b.textContent='Exit Local Service';}}\n"
    "}).catch(()=>{\n"
    "alert('Network error. Exit status unknown. Reconnect to the network and verify controller state.');\n"
    "if(b){b.disabled=false;b.textContent='Exit Local Service';}\n"
    "})}\n"
    "function doReconnect(){\n"
    "var b=document.getElementById('btn-reconnect');if(b){b.disabled=true;b.textContent='Requesting...';}\n"
    "fetch('/api/network/reconnect',{method:'POST'}).then(r=>{\n"
    "if(r.status===202){refresh();if(b){b.disabled=false;b.textContent='Reconnect Wi-Fi';}}\n"
    "else{r.json().then(d=>alert(d.error||'Reconnect request failed')).catch(()=>alert('Request failed'));if(b){b.disabled=false;b.textContent='Reconnect Wi-Fi';}}\n"
    "}).catch(()=>{\n"
    "alert('Network error during reconnect request.');\n"
    "if(b){b.disabled=false;b.textContent='Reconnect Wi-Fi';}\n"
    "})}\n"
    "refresh();\n"
    "</script>\n"
    "</body>\n"
    "</html>";

/* ── File-scope password change page HTML ────────────────────────── */

static const char PW_CHANGE_HTML[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Argus \xe2\x80\x94 Change Password</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "background:#0f1117;color:#e4e4e7;min-height:100vh;display:flex;"
    "align-items:center;justify-content:center;padding:16px}"
    ".card{background:#1a1b23;border:1px solid #27272a;border-radius:12px;"
    "padding:24px;width:100%;max-width:380px}"
    "h1{font-size:1.1rem;color:#a78bfa;margin-bottom:4px}"
    ".warn{background:#451a03;color:#fbbf24;padding:10px 12px;border-radius:8px;"
    "font-size:0.8rem;margin:12px 0}"
    "label{display:block;font-size:0.8rem;color:#a1a1aa;margin:12px 0 4px}"
    "input{width:100%;padding:10px;background:#27272a;border:1px solid #3f3f46;"
    "border-radius:8px;color:#e4e4e7;font-size:0.9rem}"
    "input:focus{outline:none;border-color:#7c3aed}"
    ".btn{background:#7c3aed;color:#fff;border:none;border-radius:8px;"
    "padding:10px 20px;font-size:0.875rem;font-weight:500;cursor:pointer;"
    "width:100%;margin-top:16px;transition:background 0.15s}"
    ".btn:active{background:#6d28d9}"
    ".btn:disabled{background:#3f3f46;cursor:not-allowed}"
    "#msg{font-size:0.8rem;margin-top:8px;min-height:1.2em}"
    ".ok{color:#34d399}.err{color:#f87171}"
    "</style></head><body>"
    "<div class=\"card\">"
    "<h1>&#9670; Argus Service Portal</h1>"
    "<div class=\"warn\">Change your portal password below.</div>"
    "<label for=\"pw1\">New Password (min 4 characters)</label>"
    "<input type=\"password\" id=\"pw1\" autocomplete=\"new-password\">"
    "<label for=\"pw2\">Confirm New Password</label>"
    "<input type=\"password\" id=\"pw2\" autocomplete=\"new-password\">"
    "<button class=\"btn\" id=\"btn\" onclick=\"submit()\">Change Password</button>"
    "<div id=\"msg\"></div>"
    "</div>"
    "<script>"
    "function submit(){"
    "var p1=document.getElementById('pw1').value;"
    "var p2=document.getElementById('pw2').value;"
    "var msg=document.getElementById('msg');"
    "var btn=document.getElementById('btn');"
    "if(p1.length<4){msg.className='err';msg.textContent='Password must be at least 4 characters';return}"
    "if(p1!==p2){msg.className='err';msg.textContent='Passwords do not match';return}"
    "if(p1==='admin'){msg.className='err';msg.textContent='Cannot reuse default password';return}"
    "btn.disabled=true;btn.textContent='Saving...';"
    "fetch('/api/portal-password',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({new_password:p1})})"
    ".then(r=>r.json()).then(d=>{"
    "if(d.ok){msg.className='ok';msg.textContent='Password changed. Reloading...';"
    "setTimeout(()=>{window.location.href='/'},1500)}"
    "else{msg.className='err';msg.textContent=d.error||'Failed';btn.disabled=false;btn.textContent='Change Password'}"
    "}).catch(e=>{msg.className='err';msg.textContent='Network error';btn.disabled=false;btn.textContent='Change Password'})}"
    "</script></body></html>";

/* ── GET /change-password handler ────────────────────────────────── */

static esp_err_t change_password_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    if (req->method != HTTP_GET) return send_method_not_allowed(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_send(req, PW_CHANGE_HTML, sizeof(PW_CHANGE_HTML) - 1);
    return ESP_OK;
}

/* ── GET /api/logout handler ─────────────────────────────────────── */

static esp_err_t logout_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Argus Service Portal\"");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{background:#0f1117;color:#e4e4e7;font-family:-apple-system,sans-serif;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh}"
        ".card{background:#1a1b23;border:1px solid #27272a;border-radius:12px;padding:24px;"
        "text-align:center;max-width:320px}"
        "h2{color:#a78bfa;font-size:1rem;margin-bottom:8px}"
        "p{color:#a1a1aa;font-size:0.85rem;margin-bottom:12px}"
        "a{color:#7c3aed;text-decoration:none}</style></head><body>"
        "<div class=\"card\"><h2>Logged Out</h2>"
        "<p>Your session has been cleared.</p>"
        "<a href=\"/\">Log in again</a></div></body></html>");
    return ESP_OK;
}

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    if (req->method != HTTP_GET) {
        return send_method_not_allowed(req);
    }

    /* Check if password needs changing */
    char stored_pw[PORTAL_MAX_PW_LEN + 1];
    bool is_default;
    portal_get_credentials(stored_pw, &is_default);
    memset(stored_pw, 0, sizeof(stored_pw));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");

    if (is_default) {
        /* Send password change page instead of portal */
        httpd_resp_send(req, PW_CHANGE_HTML, sizeof(PW_CHANGE_HTML) - 1);
        return ESP_OK;
    }

    /* Normal portal */
    httpd_resp_send(req, PORTAL_HTML, sizeof(PORTAL_HTML) - 1);
    return ESP_OK;
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
static int recv_full_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    size_t content_len = req->content_len;
    if (content_len == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"empty_body\"}");
        return -1;
    }
    if (content_len >= buf_size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "{\"error\":\"body_too_large\"}");
        /* Drain the socket to prevent connection reuse issues */
        char drain[64];
        while (httpd_req_recv(req, drain, sizeof(drain)) > 0) {}
        return -1;
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
            return -1;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return (int)received;
}

/* ── Phase 4B.2: Identity Config Page HTML ───────────────────────── */

static const char IDENTITY_PAGE_HTML[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Argus Identity</title>"
    "<style>"
    ":root{--bg:#0f1117;--card:#1a1b23;--border:#27272a;--text:#e4e4e7;--muted:#a1a1aa;"
    "--accent:#7c3aed;--accent-light:#a78bfa;--accent-dark:#6d28d9;"
    "--success:#22c55e;--warn:#f59e0b;--error:#ef4444;--input-bg:#27272a}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"
    "'Segoe UI',sans-serif;min-height:100vh;display:flex;align-items:center;"
    "justify-content:center;padding:16px}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;"
    "padding:24px;max-width:420px;width:100%}"
    "h1{color:var(--accent-light);font-size:1.1rem;margin-bottom:16px}"
    "h1::before{content:'\\25C6 ';color:var(--accent)}"
    "label{display:block;color:var(--muted);font-size:0.8rem;margin:12px 0 4px;"
    "text-transform:uppercase;letter-spacing:0.05em}"
    "input{width:100%;padding:10px 12px;background:var(--input-bg);"
    "border:1px solid var(--border);border-radius:8px;color:var(--text);"
    "font-size:0.9rem;outline:none;transition:border 0.2s}"
    "input:focus{border-color:var(--accent)}"
    "input[readonly]{opacity:0.6;cursor:not-allowed}"
    ".btn{display:block;width:100%;padding:12px;border:none;border-radius:8px;"
    "font-size:0.9rem;font-weight:600;cursor:pointer;transition:opacity 0.2s;margin-top:16px}"
    ".btn-primary{background:var(--accent);color:#fff}"
    ".btn-primary:hover{opacity:0.9}"
    ".btn-back{background:var(--border);color:var(--text);margin-top:8px;"
    "text-align:center;text-decoration:none;display:block;padding:10px;border-radius:8px;"
    "font-size:0.85rem}"
    ".alert{padding:10px 12px;border-radius:8px;font-size:0.85rem;margin-top:12px;display:none}"
    ".alert-ok{background:#052e16;border:1px solid #166534;color:#4ade80;display:block}"
    ".alert-err{background:#350a0a;border:1px solid #991b1b;color:#fca5a5;display:block}"
    ".alert-warn{background:#351f05;border:1px solid #92400e;color:#fcd34d;display:block}"
    ".locked{color:var(--warn);font-size:0.8rem;margin-bottom:12px}"
    "</style></head><body>"
    "<div class='card'><h1>Identity Configuration</h1>"
    "<div id='lock' class='locked' style='display:none'>"
    "Identity is locked. Contact Argus support to modify.</div>"
    "<form id='frm'>"
    "<label>Client ID</label>"
    "<input id='cid' type='text' maxlength='32' placeholder='e.g. acme_corp'>"
    "<label>Unit ID</label>"
    "<input id='uid' type='text' maxlength='32' placeholder='e.g. pump_001'>"
    "<label>Device Name</label>"
    "<input id='dname' type='text' maxlength='64' placeholder='e.g. Main Process Pump'>"
    "<button type='submit' class='btn btn-primary' id='btn'>Save Identity</button>"
    "</form>"
    "<div id='msg' class='alert'></div>"
    "<a href='/' class='btn-back'>Back to Dashboard</a>"
    "</div>"
    "<script>"
    "function h(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML}"
    "var locked=false,origCfg={};"
    "fetch('/api/config').then(r=>r.json()).then(d=>{"
    "origCfg=d;"
    "document.getElementById('cid').value=d.client_id||'';"
    "document.getElementById('uid').value=d.unit_id||'';"
    "document.getElementById('dname').value=d.device_name||'';"
    "if(d.identity_provisioned){"
    "locked=true;"
    "document.getElementById('lock').style.display='block';"
    "document.getElementById('cid').readOnly=true;"
    "document.getElementById('uid').readOnly=true;"
    "document.getElementById('dname').readOnly=true;"
    "document.getElementById('btn').style.display='none';"
    "}"
    "}).catch(()=>{});"
    "document.getElementById('frm').onsubmit=function(e){"
    "e.preventDefault();"
    "if(locked)return;"
    "var msg=document.getElementById('msg'),btn=document.getElementById('btn');"
    "btn.disabled=true;btn.textContent='Saving...';"
    "msg.className='alert';msg.style.display='none';"
    "fetch('/api/config/save',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({scope:'identity',"
    "client_id:document.getElementById('cid').value,"
    "unit_id:document.getElementById('uid').value,"
    "device_name:document.getElementById('dname').value})})"
    ".then(r=>r.json()).then(d=>{"
    "if(d.status==='saved'){"
    "msg.className='alert alert-ok';"
    "msg.textContent='Identity saved and locked. Restart required for changes to take effect.';"
    "msg.style.display='block';"
    "document.getElementById('cid').readOnly=true;"
    "document.getElementById('uid').readOnly=true;"
    "document.getElementById('dname').readOnly=true;"
    "btn.style.display='none';"
    "document.getElementById('lock').style.display='block';"
    "}else{"
    "msg.className='alert alert-err';"
    "var t=d.error||'Save failed';"
    "if(d.fields){var f=d.fields;for(var k in f)t+='\\n'+k+': '+f[k]}"
    "msg.textContent=t;msg.style.display='block';"
    "btn.disabled=false;btn.textContent='Save Identity'}"
    "}).catch(()=>{msg.className='alert alert-err';"
    "msg.textContent='Network error';msg.style.display='block';"
    "btn.disabled=false;btn.textContent='Save Identity'})};"
    "</script></body></html>";

/* ── Phase 4B.2: WiFi Config Page HTML ───────────────────────────── */

static const char WIFI_PAGE_HTML[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Argus WiFi</title>"
    "<style>"
    ":root{--bg:#0f1117;--card:#1a1b23;--border:#27272a;--text:#e4e4e7;--muted:#a1a1aa;"
    "--accent:#7c3aed;--accent-light:#a78bfa;--accent-dark:#6d28d9;"
    "--success:#22c55e;--warn:#f59e0b;--error:#ef4444;--input-bg:#27272a}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"
    "'Segoe UI',sans-serif;min-height:100vh;display:flex;align-items:center;"
    "justify-content:center;padding:16px}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;"
    "padding:24px;max-width:420px;width:100%}"
    "h1{color:var(--accent-light);font-size:1.1rem;margin-bottom:16px}"
    "h1::before{content:'\\25C6 ';color:var(--accent)}"
    "label{display:block;color:var(--muted);font-size:0.8rem;margin:12px 0 4px;"
    "text-transform:uppercase;letter-spacing:0.05em}"
    "input{width:100%;padding:10px 12px;background:var(--input-bg);"
    "border:1px solid var(--border);border-radius:8px;color:var(--text);"
    "font-size:0.9rem;outline:none;transition:border 0.2s}"
    "input:focus{border-color:var(--accent)}"
    ".btn{display:block;width:100%;padding:12px;border:none;border-radius:8px;"
    "font-size:0.9rem;font-weight:600;cursor:pointer;transition:opacity 0.2s;margin-top:16px}"
    ".btn-primary{background:var(--accent);color:#fff}"
    ".btn-primary:hover{opacity:0.9}"
    ".btn-danger{background:var(--error);color:#fff;margin-top:8px}"
    ".btn-back{background:var(--border);color:var(--text);margin-top:8px;"
    "text-align:center;text-decoration:none;display:block;padding:10px;border-radius:8px;"
    "font-size:0.85rem}"
    ".alert{padding:10px 12px;border-radius:8px;font-size:0.85rem;margin-top:12px;display:none}"
    ".alert-ok{background:#052e16;border:1px solid #166534;color:#4ade80;display:block}"
    ".alert-err{background:#350a0a;border:1px solid #991b1b;color:#fca5a5;display:block}"
    ".alert-warn{background:#351f05;border:1px solid #92400e;color:#fcd34d;display:block}"
    ".note{color:var(--muted);font-size:0.75rem;margin-top:8px}"
    "</style></head><body>"
    "<div class='card'><h1>WiFi Configuration</h1>"
    "<form id='frm'>"
    "<label>WiFi SSID</label>"
    "<input id='ssid' type='text' maxlength='32' placeholder='Network name'>"
    "<label>WiFi Password</label>"
    "<input id='pass' type='password' maxlength='63' placeholder='Enter WiFi password'>"
    "<p class='note' id='passnote'></p>"
    "<button type='submit' class='btn btn-primary' id='btn'>Save WiFi</button>"
    "</form>"
    "<button class='btn btn-danger' id='clrbtn' onclick='clearWifi()'>Clear WiFi</button>"
    "<div id='msg' class='alert'></div>"
    "<a href='/' class='btn-back'>Back to Dashboard</a>"
    "</div>"
    "<script>"
    "var origSsid='';"
    "fetch('/api/config').then(r=>r.json()).then(d=>{"
    "document.getElementById('ssid').value=d.sta_ssid||'';"
    "origSsid=d.sta_ssid||'';"
    "if(d.sta_pass_set){"
    "document.getElementById('pass').placeholder='Password configured (leave blank to keep)';"
    "document.getElementById('passnote').textContent='Leave password blank to keep the existing one.'}"
    "}).catch(()=>{});"
    "document.getElementById('frm').onsubmit=function(e){"
    "e.preventDefault();"
    "var msg=document.getElementById('msg'),btn=document.getElementById('btn');"
    "var ssid=document.getElementById('ssid').value;"
    "var pass=document.getElementById('pass').value;"
    "btn.disabled=true;btn.textContent='Saving...';"
    "msg.className='alert';msg.style.display='none';"
    "var body={scope:'wifi',sta_ssid:ssid};"
    "if(pass.length>0)body.sta_pass=pass;"
    "else if(ssid!==origSsid){"
    "msg.className='alert alert-err';"
    "msg.textContent='Password required when changing SSID';"
    "msg.style.display='block';"
    "btn.disabled=false;btn.textContent='Save WiFi';return}"
    "fetch('/api/config/save',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify(body)})"
    ".then(r=>r.json()).then(d=>{"
    "if(d.status==='saved'){"
    "msg.className=(d.apply_queued===false)?'alert alert-warn':'alert alert-ok';"
    "msg.textContent=d.message||'Saved.';"
    "msg.style.display='block';origSsid=ssid;"
    "}else{"
    "msg.className='alert alert-err';"
    "var t=d.error||'Save failed';"
    "if(d.fields){var f=d.fields;for(var k in f)t+='\\n'+k+': '+f[k]}"
    "msg.textContent=t;msg.style.display='block'}"
    "btn.disabled=false;btn.textContent='Save WiFi';"
    "}).catch(()=>{msg.className='alert alert-err';"
    "msg.textContent='Network error';msg.style.display='block';"
    "btn.disabled=false;btn.textContent='Save WiFi'})};"
    "function clearWifi(){"
    "if(!confirm('Clear WiFi credentials? The controller will not connect to any network after restart.'))return;"
    "var msg=document.getElementById('msg');"
    "fetch('/api/config/save',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({scope:'wifi',sta_ssid:'',sta_pass:''})})"
    ".then(r=>r.json()).then(d=>{"
    "if(d.status==='saved'){"
    "msg.className=(d.apply_queued===false||d.restart_required)?'alert alert-warn':'alert alert-ok';"
    "msg.textContent=d.message||'WiFi cleared. Restart required.';"
    "msg.style.display='block';"
    "document.getElementById('ssid').value='';"
    "document.getElementById('pass').value='';origSsid='';"
    "}else{msg.className='alert alert-err';"
    "msg.textContent=d.error||'Failed';msg.style.display='block'}"
    "}).catch(()=>{msg.className='alert alert-err';"
    "msg.textContent='Network error';msg.style.display='block'})}"
    "</script></body></html>";

/* ── Phase 4B.2: GET /api/config handler ─────────────────────────── */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
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
    if (!check_auth(req)) return ESP_OK;
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
    if (body_len < 0) return ESP_OK;  /* error response already sent */

    /* Parse scope */
    char scope_str[16] = {0};
    argus_json_result_t jr = argus_json_extract_string(body, "scope", scope_str, sizeof(scope_str));
    if (jr != ARGUS_JSON_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        const char *reason = (jr == ARGUS_JSON_KEY_ABSENT) ? "missing_scope" :
                             (jr == ARGUS_JSON_OVERFLOW) ? "scope_too_long" :
                             (jr == ARGUS_JSON_UNTERMINATED) ? "malformed_scope" :
                             "invalid_scope_type";
        char err_body[128];
        snprintf(err_body, sizeof(err_body),
                 "{\"error\":\"%s\",\"message\":\"scope must be identity or wifi\"}", reason);
        httpd_resp_sendstr(req, err_body);
        return ESP_OK;
    }
    argus_config_scope_t scope = argus_config_overlay_parse_scope(scope_str);
    if (scope == ARGUS_CONFIG_SCOPE_INVALID) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_scope\","
            "\"message\":\"scope must be identity or wifi\"}");
        return ESP_OK;
    }

    /* Parse fields from JSON body */
    argus_config_fields_t fields;
    memset(&fields, 0, sizeof(fields));

    if (scope == ARGUS_CONFIG_SCOPE_IDENTITY) {
        /* Identity fields: all three must be valid strings if present.
         * KEY_ABSENT is acceptable (overlay decides if all-three are required).
         * OVERFLOW/UNTERMINATED/TYPE_MISMATCH are malformed input → 400. */
        argus_json_result_t jr_cid = argus_json_extract_string(body, "client_id",
            fields.client_id, sizeof(fields.client_id));
        argus_json_result_t jr_uid = argus_json_extract_string(body, "unit_id",
            fields.unit_id, sizeof(fields.unit_id));
        argus_json_result_t jr_dn = argus_json_extract_string(body, "device_name",
            fields.device_name, sizeof(fields.device_name));

        if (jr_cid != ARGUS_JSON_OK && jr_cid != ARGUS_JSON_KEY_ABSENT) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"malformed_client_id\"}");
            return ESP_OK;
        }
        if (jr_uid != ARGUS_JSON_OK && jr_uid != ARGUS_JSON_KEY_ABSENT) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"malformed_unit_id\"}");
            return ESP_OK;
        }
        if (jr_dn != ARGUS_JSON_OK && jr_dn != ARGUS_JSON_KEY_ABSENT) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"malformed_device_name\"}");
            return ESP_OK;
        }
        fields.has_client_id = (jr_cid == ARGUS_JSON_OK);
        fields.has_unit_id = (jr_uid == ARGUS_JSON_OK);
        fields.has_device_name = (jr_dn == ARGUS_JSON_OK);
    } else {
        /* WiFi fields: SSID required, password optional.
         * KEY_ABSENT for password → has_sta_pass = false (preserve existing).
         * Any malformed result for a present field → 400. */
        argus_json_result_t jr_ssid = argus_json_extract_string(body, "sta_ssid",
            fields.sta_ssid, sizeof(fields.sta_ssid));
        if (jr_ssid != ARGUS_JSON_OK && jr_ssid != ARGUS_JSON_KEY_ABSENT) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"malformed_sta_ssid\"}");
            return ESP_OK;
        }
        fields.has_sta_ssid = (jr_ssid == ARGUS_JSON_OK);

        argus_json_result_t jr_pass = argus_json_extract_string(body, "sta_pass",
            fields.sta_pass, sizeof(fields.sta_pass));
        if (jr_pass != ARGUS_JSON_OK && jr_pass != ARGUS_JSON_KEY_ABSENT) {
            /* Malformed password must NOT trigger the "preserve existing" path */
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"malformed_sta_pass\"}");
            return ESP_OK;
        }
        fields.has_sta_pass = (jr_pass == ARGUS_JSON_OK);
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
    if (!check_auth(req)) return ESP_OK;
    if (req->method != HTTP_GET) return send_method_not_allowed(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_send(req, IDENTITY_PAGE_HTML, sizeof(IDENTITY_PAGE_HTML) - 1);
    return ESP_OK;
}

static esp_err_t wifi_page_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    if (req->method != HTTP_GET) return send_method_not_allowed(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_send(req, WIFI_PAGE_HTML, sizeof(WIFI_PAGE_HTML) - 1);
    return ESP_OK;
}

/* ── Phase 4B.2: POST /api/restart handler ───────────────────────── */


/* ── POST /api/network/reconnect handler ─────────────────────────── */

static esp_err_t reconnect_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
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
    if (!check_auth(req)) return ESP_OK;
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

    if (!check_auth(req)) return ESP_OK;

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

    if (!check_auth(req)) return ESP_OK;

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

/**
 * @brief POST handler to change the portal password.
 *
 * Expects JSON body: {"new_password": "<password>"}
 * Validates: min 4 chars, max 64 chars, not "admin".
 * Stores in NVS, sets pw_set flag.
 * Responds with {"ok":true} or {"ok":false,"error":"..."}
 *
 * Note: After password change, the browser's cached Basic Auth
 * credentials (admin/admin) become invalid. The next request will
 * receive 401, prompting the browser to re-authenticate.
 */
static esp_err_t password_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    set_api_headers(req);

    /* Read body */
    char body[128];
    int len = recv_full_body(req, body, sizeof(body));
    if (len < 0) return ESP_OK;  /* error response already sent */

    /* Simple JSON parse — find "new_password":"..." */
    const char *key = "\"new_password\"";
    char *kp = strstr(body, key);
    if (!kp) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing new_password\"}");
        return ESP_OK;
    }
    /* Find the value string */
    char *colon = strchr(kp + strlen(key), ':');
    if (!colon) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"malformed json\"}");
        return ESP_OK;
    }
    char *q1 = strchr(colon, '"');
    if (!q1) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"malformed json\"}");
        return ESP_OK;
    }
    q1++;  /* skip opening quote */
    char *q2 = strchr(q1, '"');
    if (!q2 || (q2 - q1) < PORTAL_MIN_PW_LEN || (q2 - q1) > PORTAL_MAX_PW_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        char err_msg[80];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"ok\":false,\"error\":\"password must be %d-%d characters\"}",
                 PORTAL_MIN_PW_LEN, PORTAL_MAX_PW_LEN);
        httpd_resp_sendstr(req, err_msg);
        return ESP_OK;
    }

    /* Extract password */
    size_t pw_len = (size_t)(q2 - q1);
    char new_pw[PORTAL_MAX_PW_LEN + 1];
    memcpy(new_pw, q1, pw_len);
    new_pw[pw_len] = '\0';

    /* Must not be the default */
    if (strcmp(new_pw, PORTAL_DEFAULT_PASS) == 0) {
        memset(new_pw, 0, sizeof(new_pw));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"cannot reuse default password\"}");
        return ESP_OK;
    }

    /* Store */
    esp_err_t err = portal_set_password(new_pw);
    memset(new_pw, 0, sizeof(new_pw));
    memset(body, 0, sizeof(body));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store portal password: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"NVS write failed\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Portal password changed successfully");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t uri_password = {
    .uri       = "/api/portal-password",
    .method    = HTTP_POST,
    .handler   = password_post_handler,
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
    config.max_uri_handlers   = 16;  /* 14 registered handlers + headroom */
    config.max_open_sockets   = HTTP_MAX_CONNECTIONS;
    config.recv_wait_timeout  = HTTP_RECV_TIMEOUT_S;
    config.send_wait_timeout  = HTTP_SEND_TIMEOUT_S;
    config.stack_size         = HTTP_STACK_SIZE;
    config.lru_purge_enable   = true;  /* Evict oldest connection if at max */
    config.server_port        = 80;
    /* Note: no open_fn or per-handler AP-subnet filtering — lwip returns
     * 0.0.0.0 for getpeername/getsockname on accepted sockets. STA-side
     * filtering deferred to Phase 4B.3. See doc block 7. */

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s (%d)", esp_err_to_name(err), err);
        s_server = NULL;
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_FAIL;
    }

    /* Register URI handlers — roll back on any failure */
    esp_err_t reg_err;
    reg_err = httpd_register_uri_handler(s_server, &uri_status);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_identity);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_portal);
    if (reg_err != ESP_OK) goto rollback;
    reg_err = httpd_register_uri_handler(s_server, &uri_password);
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
