/**
 * @file argus_http_server.c
 * @brief Controller-Hosted HTTP Server — Phase 4B.1 Read-Only Portal
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
 *    - Only GET methods accepted (405 for others).
 *    - Content-Type: application/json on all API responses.
 *    - No Internet/CDN dependency — all assets embedded.
 *
 * 7. SERVICE AP INTERFACE ENFORCEMENT
 *    In AP_DISCOVERABLE (APSTA), the HTTP server binds to INADDR_ANY and
 *    accepts connections on both interfaces. Each handler calls
 *    check_ap_subnet() which uses httpd_req_to_sockfd() + getpeername() to
 *    verify the client is in the AP subnet. Connections from non-AP subnets
 *    receive 403 Forbidden. If the peer address cannot be determined (lwip
 *    limitation), the request is served — fail-open is acceptable because
 *    this portal is read-only and serves no secrets.
 *    Note: open_fn (session-level) filtering was attempted but lwip returns
 *    0.0.0.0 for both getsockname and getpeername on the pre-handler socket.
 */

#include "argus_http_server.h"
#include "argus_identity.h"
#include "argus_state_mgr.h"
#include "argus_authority_mgr.h"
#include "argus_net_mgr.h"
#include "argus_mqtt_broker.h"
#include "argus_nvs_config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

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

/* ── AP-Subnet Enforcement (per-handler) ────────────────────────── */

/**
 * @brief Check if the request originates from the AP subnet.
 *
 * Uses httpd_req_to_sockfd() + getpeername() to get the client's IP,
 * then compares against the AP netif's subnet. Returns true if the
 * client is in the AP subnet or if the check cannot be performed
 * (fail-open: acceptable for a read-only, secret-free portal).
 *
 * Returns false only when the peer is positively identified as
 * NOT in the AP subnet.
 *
 * Note: open_fn session-level filtering was attempted but lwip returns
 * 0.0.0.0 for both getsockname() and getpeername() on the socket fd
 * passed to open_fn before the connection is fully established.
 */
static bool check_ap_subnet(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        ESP_LOGD(TAG, "AP check: cannot get socket fd, allowing (fail-open)");
        return true;  /* fail-open */
    }

    struct sockaddr_in peer_addr;
    socklen_t addr_len = sizeof(peer_addr);
    if (getpeername(sockfd, (struct sockaddr *)&peer_addr, &addr_len) != 0) {
        ESP_LOGD(TAG, "AP check: getpeername failed, allowing (fail-open)");
        return true;  /* fail-open */
    }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        ESP_LOGD(TAG, "AP check: no AP netif, allowing (fail-open)");
        return true;  /* fail-open */
    }

    esp_netif_ip_info_t ap_ip;
    if (esp_netif_get_ip_info(ap_netif, &ap_ip) != ESP_OK) {
        ESP_LOGD(TAG, "AP check: no AP IP info, allowing (fail-open)");
        return true;  /* fail-open */
    }

    uint32_t peer_subnet = peer_addr.sin_addr.s_addr & ap_ip.netmask.addr;
    uint32_t ap_subnet   = ap_ip.ip.addr & ap_ip.netmask.addr;

    if (peer_subnet != ap_subnet) {
        ESP_LOGW(TAG, "Rejected request from non-AP subnet (peer=%s)",
                 inet_ntoa(peer_addr.sin_addr));
        return false;
    }

    return true;
}

/**
 * @brief Send 403 Forbidden response.
 */
static esp_err_t send_forbidden(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"forbidden\"}");
    return ESP_OK;
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
    if (!check_ap_subnet(req)) {
        return send_forbidden(req);
    }

    if (req->method != HTTP_GET) {
        return send_method_not_allowed(req);
    }

    set_api_headers(req);

    /* Gather coherent snapshots from existing manager APIs */
    argus_state_snapshot_t state_snap;
    argus_state_mgr_get_snapshot(&state_snap);

    argus_authority_snapshot_t auth_snap;
    esp_err_t auth_err = argus_authority_mgr_get_snapshot(&auth_snap);
    if (auth_err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"authority snapshot failed\"}");
        return ESP_OK;
    }

    /* Coherent network snapshot — takes s_net_mutex (safe: see doc block 4) */
    argus_net_snapshot_t net_snap;
    esp_err_t net_err = argus_net_mgr_get_snapshot(&net_snap);
    if (net_err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"network snapshot failed\"}");
        return ESP_OK;
    }

    argus_mqtt_broker_lifecycle_obs_t broker_obs = {0};
    esp_err_t broker_err = argus_mqtt_broker_get_lifecycle_obs(&broker_obs);

    const char *machine_state_str = argus_state_mgr_get_state_name(state_snap.machine_state);
    const char *net_mode_str = argus_net_mgr_get_mode_name(net_snap.mode);
    const char *auth_mode_str = argus_authority_mgr_get_mode_name(auth_snap.mode);
    const char *auth_owner_str = argus_authority_mgr_get_owner_name(auth_snap.owner);

    /* NVS commissioned status — no secrets */
    argus_config_payload_t cfg;
    bool commissioned = (argus_nvs_config_get(&cfg) == ESP_OK) &&
                        argus_nvs_config_is_commissioned(&cfg);
    /* Zero the config to prevent accidental secret leakage */
    memset(&cfg, 0, sizeof(cfg));

    /* Build JSON response */
    char buf[768];
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
        "\"ap_started\":%s},"
        "\"broker\":{\"running\":%s,"
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
        net_snap.mqtt_broker_running ? "true" : "false",
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
    if (!check_ap_subnet(req)) {
        return send_forbidden(req);
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
    "<button class=\"refresh-btn\" onclick=\"refresh()\">Refresh Status</button>"
    "<div class=\"note\">Read-only portal. Generated speeds are not proof of physical shaft motion.</div>"
    "<script>"
    /* h() — HTML-escape to prevent DOM injection from device-supplied values */
    "function h(s){if(s==null)return'';"
    "return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')"
    ".replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;')}"
    "function row(l,v,c){return '<div class=\"row\"><span class=\"label\">'+h(l)+"
    "'</span><span class=\"val\">'+(c?'<span class=\"badge '+h(c)+'\">'+h(v)+'</span>':h(v))+'</span></div>'}"
    "function bc(s){var m={'RUNNING':'badge-ok','HOLDING':'badge-ok','UNLOCKED':'badge-warn',"
    "'STARTING':'badge-warn','DECELERATING':'badge-warn','EMERGENCY_STOPPED':'badge-err',"
    "'FAULTED':'badge-err','BOOTING':'badge-off','RECOVERING':'badge-warn'};"
    "return m[s]||'badge-off'}"
    "function nc(s){return s==='COMMISSIONED_STA'||s==='AP_DISCOVERABLE'?'badge-ok':"
    "s==='SERVICE_AP_ONLY'?'badge-warn':s==='NETWORK_FAULT'?'badge-err':'badge-off'}"
    "function fmt(v){return (v/1000).toFixed(1)+' RPM'}"
    "function refresh(){"
    "fetch('/api/status').then(r=>r.json()).then(d=>{"
    "var m=d.machine,a=d.authority,n=d.network;"
    "document.getElementById('machine-rows').innerHTML="
    "row('State',m.state,bc(m.state))+"
    "row('Target Speed',fmt(m.target_rpm_milli))+"
    "row('Applied Speed',fmt(m.applied_rpm_milli))+"
    "row('Generated Speed',fmt(m.generated_rpm_milli))+"
    "row('Driver',m.driver_enabled?'Enabled':'Disabled',m.driver_enabled?'badge-ok':'badge-off')+"
    "row('E-Stop',m.estop_latched?'LATCHED':'Clear',m.estop_latched?'badge-err':'badge-ok')+"
    "row('Ramp',m.ramp_active?'Active':'Idle',m.ramp_active?'badge-warn':'badge-off');"
    "document.getElementById('authority-rows').innerHTML="
    "row('Mode',a.mode)+"
    "row('Owner',a.owner)+"
    "row('Generation',a.generation);"
    "document.getElementById('network-rows').innerHTML="
    "row('Mode',n.mode,nc(n.mode))+"
    "row('STA Connected',n.sta_connected?'Yes':'No',n.sta_connected?'badge-ok':'badge-off')+"
    "row('AP Started',n.ap_started?'Yes':'No',n.ap_started?'badge-ok':'badge-off')+"
    "row('Broker',d.broker.running?'Running':'Stopped',d.broker.running?'badge-ok':'badge-off')+"
    "row('Commissioned',d.commissioned?'Yes':'No',d.commissioned?'badge-ok':'badge-warn');"
    "}).catch(e=>{document.getElementById('machine-rows').innerHTML="
    "row('Error','Failed to load status','badge-err')});"
    "fetch('/api/identity').then(r=>r.json()).then(d=>{"
    "document.getElementById('fw-ver').textContent=d.firmware_version+' | '+d.service_ssid;"
    "document.getElementById('identity-rows').innerHTML="
    "row('Hardware UID',d.hardware_uid)+"
    "row('Client ID',d.client_id||'(not set)')+"
    "row('Unit ID',d.unit_id||'(not set)')+"
    "row('Device Name',d.device_name||'(not set)')+"
    "row('Model',d.device_model);"
    "}).catch(e=>{})}"
    "refresh();"
    "</script>"
    "</body>"
    "</html>";

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    if (!check_ap_subnet(req)) {
        return send_forbidden(req);
    }

    if (req->method != HTTP_GET) {
        return send_method_not_allowed(req);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_send(req, PORTAL_HTML, sizeof(PORTAL_HTML) - 1);
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
    config.max_uri_handlers   = 8;
    config.max_open_sockets   = HTTP_MAX_CONNECTIONS;
    config.recv_wait_timeout  = HTTP_RECV_TIMEOUT_S;
    config.send_wait_timeout  = HTTP_SEND_TIMEOUT_S;
    config.stack_size         = HTTP_STACK_SIZE;
    config.lru_purge_enable   = true;  /* Evict oldest connection if at max */
    config.server_port        = 80;
    /* Note: open_fn was removed — lwip returns 0.0.0.0 in the pre-handler
     * socket context. AP-subnet enforcement is performed per-handler via
     * check_ap_subnet() using httpd_req_to_sockfd() + getpeername(). */

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
